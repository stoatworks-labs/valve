#include "Shaders.h"

#include "Chain.h"
#include "Valves.h"

namespace valvefx::shaders
{

//The GLSL spells these as integer literals; keep the two sides one number.
static_assert( model::kPerturbBasebandChroma == 8, "the display's kPerturbBaseband" );
static_assert( model::kPerturbBypassCarrier == 16, "the display's kPerturbBypass" );
static_assert( chain::kNodes == 4097 && chain::kCentre == 2048, "the library's table width" );
static_assert( chain::kRows == 18, "the library's WarpOf[ 18 ]" );
static_assert( chain::kTaps == 64 && chain::kTableY == 256 && chain::kTableA == 64 && chain::kTableC == 256,
               "the tables pass's grid" );

const char* const kVertex = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

namespace
{
//---------------------------------------------------------------------------
// The chain. A table is kNodes plate deviations (volts) at grid voltages
// v_i = c + w sinh( ( i - 2048 ) du ); a lookup inverts the warp, takes the
// two nodes either side by texelFetch and interpolates in float. The centre
// node is the quiescent point and holds exactly 0, so a grid at rest reads
// exactly 0 and the next stage sits at ITS rest.
//
// asinh is written out rather than called: GLSL's built-in is free to lose
// everything for large negative arguments (x + sqrt( x^2 + 1 ) cancels), and
// both signs reach a kilovolt here.
//---------------------------------------------------------------------------
const char* const kLibrary = R"(
uniform sampler2D Lut;          //4097 x 18, R32F: plate deviation, volts
uniform vec3 WarpOf[ 18 ];      //per table: c, w, 1 / du
uniform int RowOf[ 6 ];         //chain * 3 + channel -> its first table
uniform int StagesOf[ 2 ];
uniform int PowerOf[ 2 ];       //0 off, 1 single-ended, 2 push-pull
uniform float DriveOf[ 2 ];     //volts on the first grid per unit of signal
uniform float MasterOf[ 2 ];    //volts on the power grids per volt of preamp swing
uniform float VgqOf[ 2 ];       //the preamp grids' rest
uniform float Interstage;       //volts on a later grid per volt of plate swing
uniform float Rest;             //the picture level the main chain rests at

float asinhSigned( float z )
{
	float a = abs( z );
	return sign( z ) * log( a + sqrt( a * a + 1.0 ) );
}

float lookup( int row, float v )
{
	vec3 warp = WarpOf[ row ];
	float x   = 2048.0 + asinhSigned( ( v - warp.x ) / warp.y ) * warp.z;
	x         = clamp( x, 0.0, 4096.0 );
	int i     = min( int( x ), 4095 );
	float t   = x - float( i );
	float l   = texelFetch( Lut, ivec2( i, row ), 0 ).r;
	float r   = texelFetch( Lut, ivec2( i + 1, row ), 0 ).r;
	return l + t * ( r - l );
}

//`s`: the signal in video units, measured from the operating point. The
//result: the last stage's swing from rest, volts.
float chainOut( int chain, int channel, float s )
{
	int base = RowOf[ chain * 3 + channel ];
	float d;
	if( StagesOf[ chain ] > 0 )
	{
		float p = lookup( base, VgqOf[ chain ] + DriveOf[ chain ] * s );
		for( int i = 1; i < StagesOf[ chain ]; ++i )
			p = lookup( base + 1, VgqOf[ chain ] + Interstage * p );
		if( PowerOf[ chain ] == 0 )
			return p;
		d = MasterOf[ chain ] * p;
	}
	else
	{
		if( PowerOf[ chain ] == 0 )
			return DriveOf[ chain ] * s;
		d = DriveOf[ chain ] * s;
	}
	return lookup( base + 2, d );
}
)";

//---------------------------------------------------------------------------
// tables: the chain's describing functions. A subcarrier of amplitude A on
// a level is A cos( theta ); through a memoryless chain its output is
// periodic in theta, and the decoder keeps two numbers of it -- the mean
// (which lands in the luma, in Composite) and the fundamental (the chroma
// that comes out). Sampled at 64 equally spaced phases, the trapezoid rule
// is exact for every harmonic below the 63rd.
//
// Rows 0..63: Composite, Y = col / 255, A = row / 63 x Reach, through the
// main chain about Rest Level. Row 64: Y/C, A = col / 255 x Reach, through the chroma chain
// about its own rest.
//---------------------------------------------------------------------------
const char* const kTablesHead = R"(#version 410 core
)";

const char* const kTablesBody = R"(
uniform float Cosines[ 64 ];    //the cosine of 2 pi k / 64, from the CPU
uniform float Reach;            //the largest legal chroma amplitude

out vec4 fragColor;

void main()
{
	int col = int( gl_FragCoord.x );
	int row = int( gl_FragCoord.y );

	float level;
	float amplitude;
	int chain;
	if( row < 64 )
	{
		level     = float( col ) / 255.0 - Rest;
		amplitude = float( row ) / 63.0 * Reach;
		chain     = 0;
	}
	else
	{
		level     = 0.0;
		amplitude = float( col ) / 255.0 * Reach;
		chain     = 1;
	}

	float mean  = 0.0;
	float first = 0.0;
	for( int k = 0; k < 64; ++k )
	{
		float c = Cosines[ k ];
		float o = chainOut( chain, 1, level + amplitude * c );
		mean  += o;
		first += o * c;
	}
	fragColor = vec4( mean / 64.0, first * ( 2.0 / 64.0 ), 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// display. Premultiplied in, premultiplied out: a valve's curve is applied
// to the colour, not to the coverage, so the colour is divided out first
// and multiplied back after (Resolume's clips with alpha are premultiplied).
//
// RGB: each channel through its own valve set. Y/C: Y through the main
// chain; the chroma's amplitude through the chroma chain's describing
// function, its phase -- the hue -- untouched, because a memoryless chain
// has no way to move it. Composite: Y and the subcarrier through ONE chain,
// read out of the ( Y, A ) table: the luma is the mean, so curvature
// rectifies some of a strong colour into its brightness, and the chroma's
// gain is the slope of the curve at the luma it rides on.
//
// y = Offset + ( f - Ref ) Scale per channel, from the CPU (Chain.cpp
// Normalise); the chroma's amplitude is F1 x ChromaScale.
//---------------------------------------------------------------------------
const char* const kDisplayHead = R"(#version 410 core
)";

const char* const kDisplayBody = R"(
uniform sampler2D InputTexture;
uniform sampler2D Tables;       //256 x 65: ( mean, fundamental ) volts
uniform int InWidth;
uniform int InHeight;
uniform int VpX;
uniform int VpY;
uniform int VpW;
uniform int VpH;

uniform int Mode;               //0 RGB, 1 Y/C, 2 Composite
uniform float Offset;
uniform vec3 Ref;
uniform vec3 Scale;
uniform float ChromaScale;
uniform float Reach;
uniform float CarrierGain;      //the bypass control's chroma gain (Perturb only)
uniform int Perturb;
uniform float MixAmount;
uniform int ShowCurve;

out vec4 fragColor;

const vec3 kLuma  = vec3( 0.299, 0.587, 0.114 );
const float kU    = 0.492111;
const float kV    = 0.877283;
const float kAlphaFloor = 1.0 / 1024.0;
const int kPerturbBaseband = 8;
const int kPerturbBypass   = 16;

vec2 compositeAt( float level, float amplitude )
{
	float fy = clamp( level, 0.0, 1.0 ) * 255.0;
	float fa = clamp( amplitude / Reach, 0.0, 1.0 ) * 63.0;
	int iy   = min( int( fy ), 254 );
	int ia   = min( int( fa ), 62 );
	float ty = fy - float( iy );
	float ta = fa - float( ia );
	vec2 a00 = texelFetch( Tables, ivec2( iy, ia ), 0 ).rg;
	vec2 a10 = texelFetch( Tables, ivec2( iy + 1, ia ), 0 ).rg;
	vec2 a01 = texelFetch( Tables, ivec2( iy, ia + 1 ), 0 ).rg;
	vec2 a11 = texelFetch( Tables, ivec2( iy + 1, ia + 1 ), 0 ).rg;
	vec2 b0  = a00 + ty * ( a10 - a00 );
	vec2 b1  = a01 + ty * ( a11 - a01 );
	return b0 + ta * ( b1 - b0 );
}

float chromaAt( float amplitude )
{
	float f = clamp( amplitude / Reach, 0.0, 1.0 ) * 255.0;
	int i   = min( int( f ), 254 );
	float t = f - float( i );
	float l = texelFetch( Tables, ivec2( i, 64 ), 0 ).g;
	float r = texelFetch( Tables, ivec2( i + 1, 64 ), 0 ).g;
	return l + t * ( r - l );
}

vec3 toRGB( float y, vec2 c )
{
	float b = y + c.x / kU;
	float r = y + c.y / kV;
	float g = ( y - 0.299 * r - 0.114 * b ) / 0.587;
	return vec3( r, g, b );
}

float mainCurve( int channel, float x )
{
	return Offset + ( chainOut( 0, channel, x - Rest ) - Ref[ channel ] ) * Scale[ channel ];
}

vec3 process( vec3 x )
{
	if( Mode == 0 )
		return vec3( mainCurve( 0, x.r ), mainCurve( 1, x.g ), mainCurve( 2, x.b ) );

	float level  = dot( x, kLuma );
	vec2 chroma  = vec2( kU * ( x.b - level ), kV * ( x.r - level ) );
	float amp    = length( chroma );
	float y;
	vec2 c;
	if( Mode == 1 )
	{
		y = mainCurve( 1, level );
		if( ( Perturb & kPerturbBaseband ) != 0 )
			c = vec2( chainOut( 1, 1, chroma.x ), chainOut( 1, 1, chroma.y ) ) * ChromaScale;
		else
			c = amp > 0.0 ? chroma * ( chromaAt( amp ) * ChromaScale / amp ) : vec2( 0.0 );
	}
	else
	{
		bool bypass = ( Perturb & kPerturbBypass ) != 0;
		vec2 t = compositeAt( level, bypass ? 0.0 : amp );
		y      = Offset + ( t.x - Ref.g ) * Scale.g;
		if( bypass )
			c = chroma * CarrierGain;
		else
			c = amp > 0.0 ? chroma * ( t.y * ChromaScale / amp ) : vec2( 0.0 );
	}
	return toRGB( y, c );
}

//---------------------------------------------------------------------------
// Show Curve: a box in the bottom-left corner, input across, output up.
// RGB draws the three channels' curves in their own colours (one valve set
// draws white); Y/C and Composite draw the luma curve white and the
// chroma's amplitude response magenta.
//---------------------------------------------------------------------------
float curveAt( int which, float t )
{
	if( which < 3 )
	{
		if( Mode == 2 )
			return Offset + ( compositeAt( t, 0.0 ).x - Ref.g ) * Scale.g;
		return mainCurve( Mode == 0 ? which : 1, t );
	}
	if( Mode == 1 )
		return chromaAt( t * Reach ) * ChromaScale / Reach;
	return compositeAt( Rest, t * Reach ).y * ChromaScale / Reach;
}

bool onCurve( int which, int bx, int by, int side )
{
	float s  = float( side );
	float yl = clamp( curveAt( which, float( bx ) / s ), -0.05, 1.05 ) * s;
	float yr = clamp( curveAt( which, float( bx + 1 ) / s ), -0.05, 1.05 ) * s;
	float y  = float( by ) + 0.5;
	return y >= min( yl, yr ) - 0.8 && y <= max( yl, yr ) + 0.8;
}

bool overlay( int X, int Y, out vec3 colour )
{
	int small  = min( VpW, VpH );
	int side   = ( small * 32 ) / 100;
	int margin = ( small * 3 ) / 100;
	int bx     = X - margin;
	int by     = Y - margin;
	if( bx < 0 || by < 0 || bx >= side || by >= side )
		return false;

	colour      = vec3( 0.06 );
	int quarter = max( side / 4, 1 );
	if( bx % quarter == 0 || by % quarter == 0 )
		colour = vec3( 0.16 );
	if( abs( bx - by ) == 0 )
		colour = vec3( 0.24 );
	if( bx == 0 || by == 0 || bx == side - 1 || by == side - 1 )
		colour = vec3( 0.45 );

	if( Mode == 0 )
	{
		vec3 lit = vec3( onCurve( 0, bx, by, side ) ? 1.0 : 0.0, onCurve( 1, bx, by, side ) ? 1.0 : 0.0,
		                 onCurve( 2, bx, by, side ) ? 1.0 : 0.0 );
		if( lit != vec3( 0.0 ) )
			colour = lit;
	}
	else
	{
		if( onCurve( 3, bx, by, side ) )
			colour = vec3( 1.0, 0.2, 1.0 );
		if( onCurve( 1, bx, by, side ) )
			colour = vec3( 1.0 );
	}
	return true;
}

void main()
{
	int X = int( gl_FragCoord.x ) - VpX;
	int Y = int( gl_FragCoord.y ) - VpY;
	int c = clamp( ( ( 2 * X + 1 ) * InWidth ) / ( 2 * VpW ), 0, InWidth - 1 );
	int r = clamp( ( ( 2 * Y + 1 ) * InHeight ) / ( 2 * VpH ), 0, InHeight - 1 );

	vec4 src = texelFetch( InputTexture, ivec2( c, r ), 0 );
	float a  = src.a;
	vec3 x   = a > kAlphaFloor ? src.rgb / a : src.rgb;
	vec4 amplified = vec4( clamp( process( x ), 0.0, 1.0 ) * a, a );
	vec4 result    = MixAmount >= 1.0 ? amplified : mix( src, amplified, MixAmount );

	vec3 box;
	if( ShowCurve == 1 && overlay( X, Y, box ) )
		result = vec4( box, 1.0 );
	fragColor = result;
}
)";

} // namespace

const std::string& TablesSource()
{
	static const std::string source = std::string( kTablesHead ) + kLibrary + kTablesBody;
	return source;
}

const std::string& DisplaySource()
{
	static const std::string source = std::string( kDisplayHead ) + kLibrary + kDisplayBody;
	return source;
}

} // namespace valvefx::shaders
