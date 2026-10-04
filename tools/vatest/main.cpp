/**
	vatest -- render Valve offline, and read the valves back out of it.

	A stage's gain, the knee where its grid starts to conduct, the harmonics
	one valve makes and a pair cancels, the crossover of a cold pair, the hue
	Y/C keeps and the differential gain Composite makes: each has one right
	answer, from small-signal theory or the describing function. Every check
	here drives the REAL plugin class through a headless GL context and
	measures the answer out of the picture it made:

		vatest --out /tmp/frame.png     a picture, on the test card
		vatest --list                   every parameter, its kind and default
		vatest --gain                   the slope at mid-grey is the stage's
		                                small-signal gain, mu R_L / ( R_L + r_p )
		                                for each preamp valve, and the power
		                                stages' own formulas
		vatest --knee                   past 0 V the grid conducts and the slope
		                                drops by r_g / ( r_g + R_s ), where stated
		vatest --harmonics              one valve: H2/H1 = ( f''/f' ) a / 4; a
		                                matched pair: no H2, H3 kept; a mismatch
		                                of 2m: twice the H2 of m
		vatest --crossover              a cold pair notches at the centre, a hot
		                                one peaks; the centre slope is the
		                                composite load line's
		vatest --hue                    Y/C keeps the hue and compresses the
		                                amplitude by the describing function;
		                                RGB moves the hue
		vatest --dg                     Composite: chroma gain follows f'( Y ),
		                                strong colour lifts luma by f'' a^2 / 4;
		                                Y/C: chroma gain ignores luma
		vatest --polarity               every common-cathode stage inverts
		vatest --identity               no valves is a wire; a whisper of drive
		                                is the input; Mix 0 is the input; alpha
		vatest --reference              every pixel against the CPU's run of the
		                                same tables, and those against the model
		vatest --curve                  Show Curve draws the curve the picture
		                                follows
		vatest --negative               every check above can FAIL
		vatest --offline                the checks that need no GL
		vatest --bench                  the render cost
		vatest --dump-shaders DIR       the exact GLSL the plugin compiles
		vatest --pipe                   raw frames in, raw frames out

	The valves (Koren's Tube.lib), the circuit, the control laws and the
	small-signal theory are stated HERE, from the library and the
	definitions, and never read out of Valves.cpp or Controls.cpp: a constant
	typed wrong there has to show up as a failed check, not as an agreement.
	The numbers read from the plugin are its table geometry -- the warp and
	the node count -- which the error bounds need. AGENTS.md has one line per
	check on where each tolerance comes from.
*/

#include "Chain.h"
#include "Controls.h"
#include "Shaders.h"
#include "Valve.h"
#include "Valves.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model = valvefx::model;
namespace vchain = valvefx::chain;

int g_checks   = 0;
int g_failures = 0;

constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The valves, stated again from Koren's Tube.lib (normankoren.com/Audio/
// Tubemods.zip, 2001) -- the same library the plugin cites -- and his
// equations, written out here independently of Valves.cpp.
//---------------------------------------------------------------------------
struct Stated
{
	const char* name;
	bool pentode;
	double mu, ex, kg1, kg2, kp, kvb, rgi;
};

const Stated kStatedPreamps[] = {
	{ "12AX7", false, 100, 1.4, 1060, 0, 600, 300, 2000 },
	{ "12AT7", false, 60, 1.35, 460, 0, 300, 300, 2000 },
	{ "12AU7", false, 21.5, 1.3, 1180, 0, 84, 300, 2000 },
	{ "6DJ8", false, 28, 1.3, 330, 0, 320, 300, 2000 },
};
const Stated kStatedPowers[] = {
	{ "EL34", true, 11, 1.35, 650, 4200, 60, 24, 1000 },
	{ "6L6GC", true, 8.7, 1.35, 1460, 4500, 48, 12, 1000 },
	{ "KT88", true, 8.8, 1.35, 730, 4200, 32, 16, 1000 },
	{ "300B", false, 3.95, 1.4, 1550, 0, 65, 300, 1000 },
};

/// The circuit, as the README states it.
constexpr double kSupply   = 300.0;
constexpr double kRa       = 100e3;
constexpr double kLeak     = 1e6;
constexpr double kStopper  = 68e3;
constexpr double kDriver   = 47e3;
constexpr double kIdle70   = 0.70;
struct StatedPower
{
	double bplus, watts, raa;
};
const StatedPower kStatedCircuits[] = { { 450, 25, 3400 }, { 450, 30, 4000 }, { 500, 35, 4000 }, { 400, 36, 5000 } };

double acLoad()
{
	return 1.0 / ( 1.0 / kRa + 1.0 / kLeak );
}

/// Koren's plate current, as his SPICE subcircuits write it:
/// G = ( PWR( E1, EX ) + PWRS( E1, EX ) ) / KG1, and for the pentode times
/// ATAN( Vp / KVB ), with E1 = Vp/KP LOG( 1 + EXP( KP ( 1/MU + Vg / SQRT( KVB + Vp^2 ) ) ) )
/// or E1 = Vg2/KP LOG( 1 + EXP( KP ( 1/MU + Vg / Vg2 ) ) ).
double koren( const Stated& v, double eg, double ep, double screen, double scale = 1.0 )
{
	double e1;
	if( !v.pentode )
	{
		if( ep <= 0.0 )
			return 0.0;
		const double z = v.kp * ( 1.0 / v.mu + eg / std::sqrt( v.kvb + ep * ep ) );
		e1             = ep / v.kp * ( z > 30.0 ? z : std::log( 1.0 + std::exp( z ) ) );
	}
	else
	{
		const double z = v.kp * ( 1.0 / v.mu + eg / screen );
		e1             = screen / v.kp * ( z > 30.0 ? z : std::log( 1.0 + std::exp( z ) ) );
	}
	if( e1 <= 0.0 )
		return 0.0;
	const double g = ( std::pow( std::fabs( e1 ), v.ex ) + std::pow( std::fabs( e1 ), v.ex ) ) / ( v.kg1 * scale );
	return v.pentode ? g * std::atan( std::max( ep, 0.0 ) / v.kvb ) : g;
}

/// A root by bisection alone: slow, simple, and not the plugin's solver.
template< typename F >
double bisect( F&& f, double lo, double hi )
{
	double flo = f( lo );
	for( int i = 0; i < 200 && hi - lo > 1e-12 * std::max( 1.0, std::fabs( lo ) ); ++i )
	{
		const double mid = 0.5 * ( lo + hi );
		const double fm  = f( mid );
		if( ( fm < 0.0 ) == ( flo < 0.0 ) )
		{
			lo  = mid;
			flo = fm;
		}
		else
			hi = mid;
	}
	return 0.5 * ( lo + hi );
}

double statedCutoff( const Stated& v, double supply )
{
	return -std::sqrt( v.kvb + supply * supply ) / v.mu;
}

struct StatedQ
{
	double eg, vp, ip, gm, rp;
};

/// The preamp's DC operating point on B+ - Ip R_a, and its small-signal
/// gm and r_p by central differences of Koren's law (not by the plugin's
/// analytic partials).
StatedQ statedQuiescent( const Stated& v, double eg, double scale = 1.0 )
{
	StatedQ q;
	q.eg = eg;
	q.vp = bisect( [ & ]( double vp ) { return vp - kSupply + kRa * koren( v, eg, vp, 0.0, scale ); }, 0.0, kSupply );
	q.ip = koren( v, eg, q.vp, 0.0, scale );
	const double h = 1e-4;
	q.gm = ( koren( v, eg + h, q.vp, 0.0, scale ) - koren( v, eg - h, q.vp, 0.0, scale ) ) / ( 2 * h );
	q.rp = 2 * h / ( koren( v, eg, q.vp + h, 0.0, scale ) - koren( v, eg, q.vp - h, 0.0, scale ) );
	return q;
}

/// The plate's deviation on the AC load line R_a || 1M for an open-circuit
/// grid voltage through a source.
double statedPlate( const Stated& v, const StatedQ& q, double open, double source, bool gridCurrent = true, double scale = 1.0 )
{
	const double eg = ( open > 0.0 && gridCurrent ) ? open * v.rgi / ( v.rgi + source ) : open;
	const double rl = acLoad();
	const double vp = bisect( [ & ]( double x ) { return x - q.vp + rl * ( koren( v, eg, x, 0.0, scale ) - q.ip ); }, 0.0,
	                          q.vp + rl * q.ip );
	return vp - q.vp;
}

/// The power stage, restated: the bias for an idle fraction, and the
/// output for a drive.
double statedPowerBias( int valve, double idle )
{
	const Stated& v        = kStatedPowers[ valve ];
	const StatedPower& c   = kStatedCircuits[ valve ];
	const double target    = idle * c.watts / c.bplus;
	if( koren( v, 0.0, c.bplus, c.bplus ) <= target )
		return 0.0;
	return bisect( [ & ]( double eg ) { return koren( v, eg, c.bplus, c.bplus ) - target; }, -c.bplus, 0.0 );
}

double statedSeLoad( int valve )
{
	const StatedPower& c = kStatedCircuits[ valve ];
	return c.bplus * c.bplus / ( kIdle70 * c.watts );
}

double gridThrough( double open, double rgi, double source )
{
	return open > 0.0 ? open * rgi / ( rgi + source ) : open;
}

/// Push-pull: V_h = ( R_aa / 4 )( I1 - I2 ) with Vp1,2 = B+ -/+ V_h,
/// measured from its rest. `sA`, `sB` the halves' kG1 scales.
double statedPushPull( int valve, double bias, double d, double sA = 1.0, double sB = 1.0 )
{
	const Stated& v      = kStatedPowers[ valve ];
	const StatedPower& c = kStatedCircuits[ valve ];
	auto raw = [ & ]( double drive ) {
		const double g1 = gridThrough( bias + drive, v.rgi, kDriver );
		const double g2 = gridThrough( bias - drive, v.rgi, kDriver );
		return bisect(
			[ & ]( double x ) {
				return x - c.raa / 4.0 * ( koren( v, g1, c.bplus - x, c.bplus, sA ) - koren( v, g2, c.bplus + x, c.bplus, sB ) );
			},
			-c.bplus, c.bplus );
	};
	return raw( d ) - raw( 0.0 );
}

double statedSingleEnded( int valve, double bias, double d )
{
	const Stated& v      = kStatedPowers[ valve ];
	const StatedPower& c = kStatedCircuits[ valve ];
	const double r       = statedSeLoad( valve );
	const double iq      = koren( v, bias, c.bplus, c.bplus );
	const double eg      = gridThrough( bias + d, v.rgi, kDriver );
	const double vp      = bisect( [ & ]( double x ) { return x - c.bplus + r * ( koren( v, eg, x, c.bplus ) - iq ); }, 0.0,
                              c.bplus + r * iq );
	return vp - c.bplus;
}

//---------------------------------------------------------------------------
// The control laws, stated from the README.
//---------------------------------------------------------------------------
float sliderForDrive( double volts )
{
	return static_cast< float >( ( std::log10( volts ) + 2.0 ) / 4.5 );
}
float sliderForMaster( double gain )
{
	return static_cast< float >( ( std::log10( gain ) + 2.0 ) / 4.0 );
}
float sliderForIdle( double fraction )
{
	return static_cast< float >( ( fraction - 0.05 ) / 0.95 );
}
float sliderForBias( double fraction )
{
	return static_cast< float >( ( fraction - 0.05 ) / 0.9 );
}
float sliderForInterstage( double db )
{
	return static_cast< float >( ( db + 40.0 ) / 40.0 );
}
float sliderForMismatch( double m )
{
	return static_cast< float >( m / 0.25 );
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const bool software = std::getenv( "VATEST_RENDERER" ) && std::string( std::getenv( "VATEST_RENDERER" ) ) == "software";
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute anyRenderer[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	//Apple's software renderer, when asked for: the other rasteriser this
	//Mac has, for the "would this hold elsewhere" pass.
	const CGLPixelFormatAttribute softwareOnly[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( software )
	{
		if( CGLChoosePixelFormat( softwareOnly, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( anyRenderer, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Valve::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Valve& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Valve::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range. An integer's is its declared range.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		if( p.type == FF_TYPE_INTEGER )
		{
			p.low  = plugin.GetParamRange( i ).min;
			p.high = plugin.GetParamRange( i ).max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Valve& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Valve& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Valve& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	std::abort();
}

/// A neutral starting point every check builds on: one 12AX7 stage, no power
/// stage, Plate output as wired, RGB, nothing else in the way.
void quiet( Valve& p )
{
	set( p, "Signal", 0 );
	set( p, "Rest Level", 0.5f );
	set( p, "Interstage", sliderForInterstage( -20.0 ) );
	set( p, "Mismatch", 0 );
	set( p, "Stages", 1 );
	set( p, "Preamp", model::k12AX7 );
	set( p, "Bias", 0.5f );
	set( p, "Power Stage", 0 );
	set( p, "Power Valve", model::kEL34 );
	set( p, "Master", sliderForMaster( 1.0 ) );
	set( p, "Power Bias", sliderForIdle( 0.7 ) );
	set( p, "Chroma Stages", 0 );
	set( p, "Chroma Pwr Stage", 0 );
	set( p, "Output", static_cast< float >( vchain::Output::Plate ) );
	set( p, "Polarity", 1 );
	set( p, "Show Curve", 0 );
	set( p, "Mix", 1 );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and its output.
//---------------------------------------------------------------------------
struct Session
{
	Valve plugin;
	int width        = 0;
	int height       = 0;
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	bool renderUploaded()
	{
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed\n" );
		return ok;
	}

	bool render( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderUploaded();
	}

	bool render( const std::vector< float >& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderUploaded();
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	/// Render a float picture and read the float result back.
	std::vector< float > through( const std::vector< float >& pixels )
	{
		if( !render( pixels ) )
			return {};
		return readBackFloat();
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

bool g_quiet = false;

int report( bool ok, const char* format, ... ) __attribute__( ( format( printf, 2, 3 ) ) );
int report( bool ok, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( !g_quiet )
	{
		va_list args;
		va_start( args, format );
		std::vprintf( format, args );
		va_end( args );
		std::printf( "  %s\n", verdict( ok ) );
	}
	return ok ? 0 : 1;
}

void note( const char* format, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
void note( const char* format, ... )
{
	if( g_quiet )
		return;
	va_list args;
	va_start( args, format );
	std::vprintf( format, args );
	va_end( args );
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first.
//---------------------------------------------------------------------------
std::vector< float > blank( int W, int H )
{
	std::vector< float > p( static_cast< size_t >( W ) * H * 4, 0.0f );
	for( size_t i = 3; i < p.size(); i += 4 )
		p[ i ] = 1.0f;
	return p;
}

void put( std::vector< float >& p, int W, int x, int y, double r, double g, double b, double a = 1.0 )
{
	float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
	px[ 0 ]   = static_cast< float >( r );
	px[ 1 ]   = static_cast< float >( g );
	px[ 2 ]   = static_cast< float >( b );
	px[ 3 ]   = static_cast< float >( a );
}

float at( const std::vector< float >& img, int W, int y, int x, int ch = 0 )
{
	return img[ ( static_cast< size_t >( y ) * W + x ) * 4 + ch ];
}

/// The signal at column x of a ramp: pixel centres from 0 to 1.
double rampAt( int x, int W )
{
	return ( x + 0.5 ) / W;
}

/// Every row a grey ramp, black at the left to white at the right.
std::vector< float > ramp( int W, int H )
{
	std::vector< float > p = blank( W, H );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const double v = rampAt( x, W );
			put( p, W, x, y, v, v, v );
		}
	return p;
}

/// PCG output mix: exact in 32 bits, the same on every machine.
uint32_t hashInt( uint32_t v )
{
	uint32_t state = v * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

/// Random colour in [0, 1], on a 1/1024 grid so the float is exact.
std::vector< float > noise( int W, int H, uint32_t seed )
{
	std::vector< float > p = blank( W, H );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			double c[ 3 ];
			for( int k = 0; k < 3; ++k )
				c[ k ] = ( hashInt( seed * 0x9E3779B9u ^ hashInt( static_cast< uint32_t >( ( y * W + x ) * 3 + k ) ) ) % 1024u ) / 1023.0;
			put( p, W, x, y, c[ 0 ], c[ 1 ], c[ 2 ] );
		}
	return p;
}

//---------------------------------------------------------------------------
// Y'UV, BT.601, stated: U = 0.492111 ( B - Y ), V = 0.877283 ( R - Y ).
//---------------------------------------------------------------------------
constexpr double kKu = 0.492111;
constexpr double kKv = 0.877283;

void toYUV( double r, double g, double b, double& y, double& u, double& v )
{
	y = 0.299 * r + 0.587 * g + 0.114 * b;
	u = kKu * ( b - y );
	v = kKv * ( r - y );
}

void fromYUV( double y, double u, double v, double& r, double& g, double& b )
{
	b = y + u / kKu;
	r = y + v / kKv;
	g = ( y - 0.299 * r - 0.114 * b ) / 0.587;
}

//---------------------------------------------------------------------------
// The card: what --out, the sweep and the bench render. Colour bars, a grey
// ramp, a saturation sweep at six hues, a skin-tone gradient and a soft
// spot, so every control has something to bite on.
//---------------------------------------------------------------------------
std::vector< float > buildCard( int W, int H )
{
	std::vector< float > p = blank( W, H );
	const double bars[ 7 ][ 3 ] = { { 0.75, 0.75, 0.75 }, { 0.75, 0.75, 0 }, { 0, 0.75, 0.75 }, { 0, 0.75, 0 },
		                            { 0.75, 0, 0.75 },    { 0.75, 0, 0 },    { 0, 0, 0.75 } };
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const double fx = ( x + 0.5 ) / W, fy = ( y + 0.5 ) / H;
			double r = 0.06, g = 0.06, b = 0.06;
			if( fy < 0.28 )
			{
				const double* c = bars[ std::min( 6, x * 7 / W ) ];
				r = c[ 0 ], g = c[ 1 ], b = c[ 2 ];
			}
			else if( fy < 0.40 )
				r = g = b = fx;
			else if( fy < 0.62 )
			{
				//Six hues across, saturation rising down the band.
				const double hue = std::floor( fx * 6.0 ) / 6.0 * 2.0 * kPi + 0.3;
				const double sat = ( fy - 0.40 ) / 0.22;
				const double a   = 0.30 * sat;
				fromYUV( 0.5, a * std::cos( hue ), a * std::sin( hue ), r, g, b );
			}
			else
			{
				//A warm gradient and a soft bright spot: something like skin
				//and something like a light.
				const double t  = fx;
				r               = 0.15 + 0.75 * t;
				g               = 0.10 + 0.52 * t;
				b               = 0.08 + 0.38 * t;
				const double dx = ( fx - 0.72 ) * W / H, dy = fy - 0.81;
				const double spot = std::exp( -( dx * dx + dy * dy ) / 0.006 );
				r += ( 1.0 - r ) * spot;
				g += ( 1.0 - g ) * spot;
				b += ( 1.0 - b ) * spot;
			}
			put( p, W, x, y, std::clamp( r, 0.0, 1.0 ), std::clamp( g, 0.0, 1.0 ), std::clamp( b, 0.0, 1.0 ) );
		}
	return p;
}

std::vector< unsigned char > toBytes( const std::vector< float >& f )
{
	std::vector< unsigned char > out( f.size() );
	for( size_t i = 0; i < f.size(); ++i )
		out[ i ] = static_cast< unsigned char >( std::lround( std::clamp( f[ i ], 0.0f, 1.0f ) * 255.0f ) );
	return out;
}


//===========================================================================
// CHECKS
//===========================================================================

//---------------------------------------------------------------------------
// The rig: one plugin instance at one raster, quieted, with settings on top.
//---------------------------------------------------------------------------
using Knobs = std::vector< std::pair< const char*, float > >;

struct Rig
{
	Session s;
	bool ok = false;

	Rig( int W, int H, int perturb )
	{
		ok = s.begin( W, H );
		s.plugin.SetPerturbForTest( perturb );
		quiet( s.plugin );
	}
	~Rig()
	{
		if( ok )
			s.end();
	}
	void apply( const Knobs& knobs )
	{
		for( const auto& k : knobs )
			set( s.plugin, k.first, k.second );
	}
	std::vector< float > run( const std::vector< float >& picture, const Knobs& knobs = {} )
	{
		apply( knobs );
		return s.through( picture );
	}
	const vchain::Tables& tables()
	{
		return s.plugin.TablesForTest();
	}
};

std::vector< double > rowOf( const std::vector< float >& img, int W, int y, int ch )
{
	std::vector< double > r( static_cast< size_t >( W ) );
	for( int x = 0; x < W; ++x )
		r[ static_cast< size_t >( x ) ] = at( img, W, y, x, ch );
	return r;
}

double ulpOf( double v )
{
	const float f = static_cast< float >( std::fabs( v ) );
	return static_cast< double >( std::nextafter( f, INFINITY ) - f );
}

/// One float ULP of 1.0, the unit every output-side error is counted in.
constexpr double kUlp1 = 1.0 / 8388608.0;

/// The drive the plugin actually has for a slider: the README's law applied
/// to the float the slider holds.
double driveOf( float slider )
{
	return std::pow( 10.0, -2.0 + 4.5 * static_cast< double >( slider ) );
}

/// The worst error of linear interpolation in the table's own coordinate
/// u (the lookup is linear in the INDEX, not in volts) over the segments
/// that cover [vlo, vhi], from the harness's own model of the stage: the
/// deviation at each segment's midpoint in u, which is the maximum to
/// leading order, plus a quarter for the next order, plus the half ULP
/// each stored float node carries. Valid where the curve is smooth inside a
/// segment -- the one kink, the grid's knee, is on a node.
double interpBound( const std::function< double( double ) >& P, const vchain::Warp& w, double vlo, double vhi )
{
	auto index = [ & ]( double v ) { return vchain::kCentre + std::asinh( ( v - w.c ) / w.w ) / w.du; };
	const int i0 = std::clamp( static_cast< int >( std::floor( index( std::min( vlo, vhi ) ) ) ) - 1, 0, vchain::kNodes - 2 );
	const int i1 = std::clamp( static_cast< int >( std::ceil( index( std::max( vlo, vhi ) ) ) ) + 1, 1, vchain::kNodes - 1 );
	double worst = 0.0;
	for( int i = i0; i < i1; ++i )
	{
		const double l   = P( w.At( i ) );
		const double r   = P( w.At( i + 1 ) );
		const double mid = P( w.c + w.w * std::sinh( ( i + 0.5 - vchain::kCentre ) * w.du ) );
		worst            = std::max( worst, 1.25 * std::fabs( mid - 0.5 * ( l + r ) ) + 0.5 * std::max( ulpOf( l ), ulpOf( r ) ) );
	}
	return worst;
}

/// What the GPU's float arithmetic can move one lookup by, given the error
/// already in its input voltage: the index is formed in float near 2048..4096
/// (half an ULP there is 2^-12), from an asinh whose log the GLSL spec allows
/// a few ULP (counted as 2^-20 ( 1 + |u| ) here, generous), and from a
/// voltage known only to `dv`; times the step between the two nodes, plus
/// the lerp's own rounding.
double lookupFloatError( const vchain::Tables& t, int row, double v, double dv )
{
	const vchain::WarpF wf = t.RowWarp( row );
	const double z         = ( v - wf.c ) / wf.w;
	const double u         = std::asinh( z );
	const double x         = std::clamp( vchain::kCentre + u * wf.invDu, 0.0, static_cast< double >( vchain::kNodes - 1 ) );
	const int i            = std::min( static_cast< int >( x ), vchain::kNodes - 2 );
	const auto& L          = t.Row( row ).values;
	const double l = L[ static_cast< size_t >( i ) ], r = L[ static_cast< size_t >( i + 1 ) ];
	const double dx = 1.0 / 4096.0 + wf.invDu * ( std::pow( 2.0, -20 ) * ( 1.0 + std::fabs( u ) ) + dv / wf.w / std::sqrt( 1.0 + z * z ) );
	return dx * std::fabs( r - l ) + 2.0 * ulpOf( std::max( std::fabs( l ), std::fabs( r ) ) );
}

/// The chain's float error at signal s, volts, walked stage by stage the way
/// the shader computes it.
double chainFloatError( const vchain::Tables& t, int chain, int channel, double s )
{
	const vchain::ChainSettings& cs = t.Current().chain[ static_cast< size_t >( chain ) ];
	const int base                  = t.RowBase( chain, channel );
	const double vg = t.Vgq( chain ), k = t.Current().interstage;
	if( cs.stages == 0 && cs.power == model::PowerStage::Off )
		return 2.0 * ulpOf( cs.drive * s );
	double err = 0.0, d = 0.0;
	if( cs.stages > 0 )
	{
		double v = vg + cs.drive * s;
		err      = lookupFloatError( t, base + vchain::kFirst, v, 2.0 * ulpOf( v ) + ulpOf( cs.drive * s ) );
		double p = t.Lookup( base + vchain::kFirst, v );
		for( int i = 1; i < cs.stages; ++i )
		{
			v   = vg + k * p;
			err = lookupFloatError( t, base + vchain::kLater, v, k * err + 2.0 * ulpOf( v ) );
			p   = t.Lookup( base + vchain::kLater, v );
		}
		if( cs.power == model::PowerStage::Off )
			return err;
		d   = cs.master * p;
		err = cs.master * err + ulpOf( d );
	}
	else
	{
		d   = cs.drive * s;
		err = 2.0 * ulpOf( d );
	}
	return lookupFloatError( t, base + vchain::kPower, d, err );
}

//---------------------------------------------------------------------------
// The harness's own model of a whole chain, in volts, for the settings the
// plugin was last given: its own Koren, its own operating points, its own
// solves.
//---------------------------------------------------------------------------
struct ModelChain
{
	int stages = 1;
	const Stated* valve = nullptr;
	StatedQ q {};
	double scale = 1.0;
	double drive = 1.0, interstage = 0.1, master = 1.0;
	int power = 0;///< 0 off, 1 SE, 2 PP
	int powerValve = 0;
	double powerBias = 0.0;
};

ModelChain modelOf( const vchain::ChainSettings& cs, double interstage, double scale )
{
	ModelChain m;
	m.stages     = cs.stages;
	m.valve      = &kStatedPreamps[ cs.preamp ];
	m.scale      = scale;
	m.q          = statedQuiescent( *m.valve, statedCutoff( *m.valve, kSupply ) * ( 1.0 - cs.bias ), scale );
	m.drive      = cs.drive;
	m.interstage = interstage;
	m.master     = cs.master;
	m.power      = static_cast< int >( cs.power );
	m.powerValve = cs.powerValve;
	m.powerBias  = statedPowerBias( cs.powerValve, cs.idle );
	return m;
}

/// A central difference, per unit of whatever f takes.
template< typename F >
double derivative( F&& f, double s, double h )
{
	return ( f( s + h ) - f( s - h ) ) / ( 2.0 * h );
}

//---------------------------------------------------------------------------
// --gain
//
// The slope of the picture at mid-grey, in Plate mode as wired, is the
// stage's small-signal gain times Drive over the supply. Small-signal theory
// says that gain is -mu R_L / ( R_L + r_p ) for a common-cathode triode with
// its cathode bypassed, -gm ( R || r_p ) for a single-ended stage into its
// transformer, and ( R_aa/4 )( gm1 + gm2 ) / ( 1 + ( R_aa/4 )( 1/r_p1 + 1/r_p2 ) )
// for a push-pull pair on its composite load line -- gm and r_p differenced
// out of Koren's law at the operating point the harness solves for itself.
// The picture's slope is a central difference across the centre; its
// distance from the derivative is the model's own curvature, computed, and
// the table's and the float's errors are bounded from the plugin's table
// geometry.
//---------------------------------------------------------------------------
struct GainCase
{
	std::string what;
	double theory;      ///< signed small-signal gain, volts per volt of grid
	double drive;       ///< volts per unit of signal
	double supply;      ///< Plate mode's divisor
	std::function< double( double ) > model;///< the harness's own stage, volts against signal
	int row;            ///< the plugin's table for the bound
	double vAtZero;     ///< the table's input at s = 0
};

int checkGain( Rig& rig, const GainCase& c, int W, int H )
{
	const std::vector< float > img = rig.run( ramp( W, H ) );
	if( img.empty() )
		return report( false, "gain %-22s: render failed", c.what.c_str() );
	const std::vector< double > row = rowOf( img, W, H / 2, 1 );
	const int m      = std::max( 1, static_cast< int >( std::lround( 0.01 * W - 0.5 ) ) );
	const double h   = ( m + 0.5 ) / W;
	const double lo  = row[ static_cast< size_t >( W / 2 - 1 - m ) ];
	const double hi  = row[ static_cast< size_t >( W / 2 + m ) ];
	const double measured = ( hi - lo ) / ( 2.0 * h ) * c.supply / c.drive;

	const double cd   = derivative( c.model, 0.0, h ) / c.drive;
	const double bias = std::fabs( cd - c.theory );
	const vchain::Tables& t = rig.tables();
	const double interp = interpBound( [ & ]( double v ) { return c.model( ( v - c.vAtZero ) / c.drive ); }, t.Row( c.row ).warp,
	                                   c.vAtZero - c.drive * h, c.vAtZero + c.drive * h );
	const double fl   = lookupFloatError( t, c.row, c.vAtZero + c.drive * h, 2.0 * ulpOf( c.vAtZero ) + ulpOf( c.drive * h ) );
	const double tol  = bias + ( interp + fl + c.supply * 4.0 * kUlp1 ) / ( h * c.drive );
	const bool clipped = lo <= 1e-6 || hi >= 1.0 - 1e-6;
	return report( !clipped && std::fabs( measured - c.theory ) <= tol, "gain %-22s: %9.4f against %9.4f, tolerance %.2g (curvature %.2g)",
	               c.what.c_str(), measured, c.theory, tol, bias );
}

int runGain( int W, int H, int perturb )
{
	Rig rig( W, H, perturb );
	if( !rig.ok )
		return report( false, "gain: could not start the plugin" );
	int failures = 0;
	note( "gain: the picture's slope at mid-grey against small-signal theory (Plate, as wired)\n" );

	for( int p = 0; p < model::kPreampCount; ++p )
	{
		const Stated& v    = kStatedPreamps[ p ];
		const double cut   = statedCutoff( v, kSupply );
		const StatedQ q    = statedQuiescent( v, cut * 0.5 );
		const double rl    = acLoad();
		const double mu    = q.gm * q.rp;
		const float slider = sliderForDrive( std::fabs( cut ) / 10.0 );
		rig.apply( { { "Preamp", static_cast< float >( p ) }, { "Stages", 1 }, { "Power Stage", 0 }, { "Bias", sliderForBias( 0.5 ) },
		             { "Drive", slider } } );
		const double drive = driveOf( slider );
		GainCase c;
		c.what    = std::string( v.name ) + " stage";
		c.theory  = -mu * rl / ( rl + q.rp );
		c.drive   = drive;
		c.supply  = kSupply;
		c.model   = [ &v, q, drive ]( double s ) { return statedPlate( v, q, q.eg + drive * s, kStopper ); };
		c.row     = vchain::RowIndex( vchain::kMain, 1, vchain::kFirst );
		c.vAtZero = q.eg;
		failures += checkGain( rig, c, W, H );
	}

	for( int pv = 0; pv < model::kPowerCount; ++pv )
		for( int stage = 1; stage <= 2; ++stage )
		{
			const Stated& v      = kStatedPowers[ pv ];
			const StatedPower& k = kStatedCircuits[ pv ];
			const double bias    = statedPowerBias( pv, kIdle70 );
			const double h       = 1e-3;
			const double gm      = ( koren( v, bias + h, k.bplus, k.bplus ) - koren( v, bias - h, k.bplus, k.bplus ) ) / ( 2 * h );
			const double rp      = 2 * h / ( koren( v, bias, k.bplus + h, k.bplus ) - koren( v, bias, k.bplus - h, k.bplus ) );
			const float slider   = sliderForDrive( std::fabs( bias ) / 10.0 );
			rig.apply( { { "Stages", 0 }, { "Power Stage", static_cast< float >( stage ) }, { "Power Valve", static_cast< float >( pv ) },
			             { "Power Bias", sliderForIdle( kIdle70 ) }, { "Drive", slider } } );
			const double drive = driveOf( slider );
			GainCase c;
			if( stage == 1 )
			{
				const double r = statedSeLoad( pv );
				c.theory       = -gm * r * rp / ( r + rp );
				c.what         = std::string( v.name ) + " single-ended";
				c.model        = [ pv, bias, drive ]( double s ) { return statedSingleEnded( pv, bias, drive * s ); };
			}
			else
			{
				const double q4 = k.raa / 4.0;
				c.theory        = q4 * 2.0 * gm / ( 1.0 + q4 * 2.0 / rp );
				c.what          = std::string( v.name ) + " push-pull";
				c.model         = [ pv, bias, drive ]( double s ) { return statedPushPull( pv, bias, drive * s ); };
			}
			c.drive   = drive;
			c.supply  = k.bplus;
			c.row     = vchain::RowIndex( vchain::kMain, 1, vchain::kPower );
			c.vAtZero = 0.0;
			failures += checkGain( rig, c, W, H );
		}
	return failures;
}

//---------------------------------------------------------------------------
// --knee
//
// Below 0 V the grid draws nothing and its voltage is the source's. Above,
// it conducts through r_g (Koren's RGI) and the source resistance R_s
// divides it: the grid moves r_g / ( r_g + R_s ) as fast. The plate's own
// slope against the grid is continuous there, so the PICTURE's slope drops
// by exactly that ratio across the knee, and the knee sits where the grid
// crosses 0: at s = -eg_q / Drive. Drive is chosen to put it at 3/4 of the
// ramp, between two pixel centres; the one-pixel slopes either side are
// held to the ratio, with the model's own curvature over a pixel and a
// half as the tolerance. The steepest bend in the whole ramp must be there.
//---------------------------------------------------------------------------
int runKnee( int W, int H, int perturb )
{
	Rig rig( W, H, perturb );
	if( !rig.ok )
		return report( false, "knee: could not start the plugin" );
	if( W % 4 != 0 )
		return report( false, "knee: the raster's width must be a multiple of 4 (the knee sits between two pixels)" );
	int failures = 0;
	for( int p : { static_cast< int >( model::k12AX7 ), static_cast< int >( model::k12AU7 ) } )
	{
		const Stated& v    = kStatedPreamps[ p ];
		const StatedQ q    = statedQuiescent( v, statedCutoff( v, kSupply ) * 0.5 );
		const double rho   = v.rgi / ( v.rgi + kStopper );
		//The knee at exactly 3/4 of the ramp needs Drive = -eg / 0.25. The
		//slider holds a float, so the knee lands a hair off; that hair is in
		//the model's own expectation below.
		const float slider = sliderForDrive( -q.eg / 0.25 );
		const double drive = driveOf( slider );
		const std::vector< float > img =
			rig.run( ramp( W, H ), { { "Preamp", static_cast< float >( p ) }, { "Stages", 1 }, { "Power Stage", 0 }, { "Bias", sliderForBias( 0.5 ) },
		                             { "Drive", slider } } );
		const std::vector< double > y = rowOf( img, W, H / 2, 1 );
		const int k0 = 3 * W / 4 - 1, k1 = 3 * W / 4;
		const double left  = ( y[ static_cast< size_t >( k0 ) ] - y[ static_cast< size_t >( k0 - 1 ) ] ) * W;
		const double right = ( y[ static_cast< size_t >( k1 + 1 ) ] - y[ static_cast< size_t >( k1 ) ] ) * W;
		const double ratio = right / left;

		auto plate = [ & ]( int x ) { return statedPlate( v, q, q.eg + drive * ( rampAt( x, W ) - 0.5 ), kStopper ) / kSupply; };
		const double mLeft  = ( plate( k0 ) - plate( k0 - 1 ) ) * W;
		const double mRight = ( plate( k1 + 1 ) - plate( k1 ) ) * W;
		const double bias   = std::fabs( mRight / mLeft - rho );
		//Each one-pixel difference is two readings, each good to the table
		//and the float.
		const vchain::Tables& t = rig.tables();
		const int row           = vchain::RowIndex( vchain::kMain, 1, vchain::kFirst );
		const double interp     = interpBound( [ & ]( double vg ) { return statedPlate( v, q, vg, kStopper ); }, t.Row( row ).warp,
		                                       q.eg + drive * ( rampAt( k0 - 1, W ) - 0.5 ), q.eg + drive * ( rampAt( k1 + 1, W ) - 0.5 ) );
		const double fl  = lookupFloatError( t, row, 0.0, 4.0 * ulpOf( q.eg ) );
		const double per = 2.0 * ( ( interp + fl ) / kSupply + 4.0 * kUlp1 ) * W;
		const double tol = bias + std::fabs( ratio ) * ( per / std::fabs( right ) + per / std::fabs( left ) );

		int sharpest = 1;
		double worst = 0.0;
		for( int x = 1; x + 1 < W; ++x )
		{
			const double bend = std::fabs( y[ static_cast< size_t >( x + 1 ) ] - 2.0 * y[ static_cast< size_t >( x ) ] + y[ static_cast< size_t >( x - 1 ) ] );
			if( bend > worst )
			{
				worst    = bend;
				sharpest = x;
			}
		}
		failures += report( std::fabs( ratio - rho ) <= tol, "knee %-6s: slope ratio across the knee %.5f against r_g/(r_g+R_s) = %.5f, tolerance %.2g",
		                    v.name, ratio, rho, tol );
		failures += report( sharpest == k0 || sharpest == k1, "knee %-6s: the sharpest bend is at column %d; the grid crosses 0 V between %d and %d",
		                    v.name, sharpest, k0, k1 );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --harmonics
//
// A cosine grating, 40 pixels a cycle, read back with a DFT at the grating's
// harmonics -- exact for a periodic function sampled 40 times a cycle up to
// aliasing above the 20th, far below anything measured here.
//
// One valve at small drive: Taylor gives H2/H1 = ( f2 / f1 ) a / 4 and
// H3/H1 = ( f3 / f1 ) a^2 / 24, fn the plate's nth derivative against the
// grid at rest; the remainder is the model's own DFT minus the Taylor terms.
// A matched push-pull pair: the stage is odd about rest, so H2 is below
// what the float's asymmetric rounding of the index can make, while H3 is
// plainly there. A mismatch m: H2 is first order in m, so 2m gives twice
// the H2 of m, to within the model's own second-order term.
//---------------------------------------------------------------------------
std::vector< float > grating( int W, int H, double amplitude, int cycles )
{
	std::vector< float > p = blank( W, H );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const double v = 0.5 + amplitude * std::cos( 2.0 * kPi * cycles * ( x + 0.5 ) / W );
			put( p, W, x, y, v, v, v );
		}
	return p;
}

double harmonic( const std::vector< double >& row, int cycles, int k )
{
	const int W = static_cast< int >( row.size() );
	std::complex< double > sum = 0.0;
	for( int x = 0; x < W; ++x )
		sum += row[ static_cast< size_t >( x ) ] * std::polar( 1.0, -2.0 * kPi * k * cycles * ( x + 0.5 ) / W );
	return 2.0 * std::abs( sum ) / W;
}

std::vector< double > sampled( const std::function< double( double ) >& f, int W, double amplitude, int cycles )
{
	std::vector< double > r( static_cast< size_t >( W ) );
	for( int x = 0; x < W; ++x )
		r[ static_cast< size_t >( x ) ] = f( static_cast< float >( 0.5 + amplitude * std::cos( 2.0 * kPi * cycles * ( x + 0.5 ) / W ) ) - 0.5 );
	return r;
}

int runHarmonics( int W, int H, int perturb )
{
	Rig rig( W, H, perturb );
	if( !rig.ok )
		return report( false, "harmonics: could not start the plugin" );
	if( W % 40 != 0 )
		return report( false, "harmonics: the raster's width must be a multiple of 40" );
	const int cycles = W / 40;
	const double a   = 0.4;
	int failures     = 0;

	//--- one valve: Taylor.
	{
		const Stated& v    = kStatedPreamps[ model::k12AX7 ];
		const StatedQ q    = statedQuiescent( v, statedCutoff( v, kSupply ) * 0.5 );
		//0.3 V of grid swing: small enough that Taylor's remainder is a few
		//per cent of H2, big enough that H3 stands well clear of the floor.
		const float slider = sliderForDrive( 0.75 );
		const double drive = driveOf( slider );
		const std::vector< float > img =
			rig.run( grating( W, H, a, cycles ), { { "Stages", 1 }, { "Power Stage", 0 }, { "Drive", slider } } );
		const std::vector< double > y = rowOf( img, W, H / 2, 1 );
		const double h1 = harmonic( y, cycles, 1 );
		const double r2 = harmonic( y, cycles, 2 ) / h1;
		const double r3 = harmonic( y, cycles, 3 ) / h1;

		auto P = [ & ]( double vg ) { return statedPlate( v, q, vg, kStopper ); };
		const double av = drive * a;
		const double d1 = ( P( q.eg + 1e-3 ) - P( q.eg - 1e-3 ) ) / 2e-3;
		const double d2 = ( P( q.eg + 1e-2 ) - 2 * P( q.eg ) + P( q.eg - 1e-2 ) ) / 1e-4;
		const double d3 = ( P( q.eg + 2e-2 ) - 2 * P( q.eg + 1e-2 ) + 2 * P( q.eg - 1e-2 ) - P( q.eg - 2e-2 ) ) / ( 2 * 1e-6 );
		const double t2 = std::fabs( d2 / d1 ) * av / 4.0;
		const double t3 = std::fabs( d3 / d1 ) * av * av / 24.0;
		const std::vector< double > m = sampled( [ & ]( double s ) { return P( q.eg + drive * s ); }, W, a, cycles );
		const double m2 = harmonic( m, cycles, 2 ) / harmonic( m, cycles, 1 );
		const double m3 = harmonic( m, cycles, 3 ) / harmonic( m, cycles, 1 );
		//The picture's error as a fraction of H1: the table and the float on
		//each of W samples can move a DFT bin by at most twice their worst.
		const vchain::Tables& t = rig.tables();
		const int row = vchain::RowIndex( vchain::kMain, 1, vchain::kFirst );
		const double e = interpBound( P, t.Row( row ).warp, q.eg - av, q.eg + av ) + lookupFloatError( t, row, q.eg + av, 4 * ulpOf( q.eg ) )
		                 + kSupply * 4 * kUlp1;
		const double floor = 2.0 * e / kSupply / h1;
		//A tolerance as big as the thing measured would pass anything: each
		//must be under a quarter of its value to count.
		const double tol2 = std::fabs( m2 - t2 ) + floor, tol3 = std::fabs( m3 - t3 ) + floor;
		failures += report( std::fabs( r2 - t2 ) <= tol2 && tol2 < 0.25 * t2,
		                    "harmonics 12AX7 alone: H2/H1 %.5f against ( f2/f1 ) a/4 = %.5f, tolerance %.2g", r2, t2, tol2 );
		failures += report( std::fabs( r3 - t3 ) <= tol3 && tol3 < 0.25 * t3,
		                    "harmonics 12AX7 alone: H3/H1 %.6f against ( f3/f1 ) a^2/24 = %.6f, tolerance %.2g", r3, t3, tol3 );
		failures += report( r2 > 4.0 * r3, "harmonics 12AX7 alone: single-ended, the second harmonic dominates (H2/H3 = %.1f)", r2 / r3 );
	}

	//--- a matched pair, and a mismatched one.
	{
		const int pv       = model::kEL34;
		const double bias  = statedPowerBias( pv, kIdle70 );
		const float slider = sliderForDrive( 0.6 * std::fabs( bias ) / a );
		const double drive = driveOf( slider );
		const double bplus = kStatedCircuits[ pv ].bplus;
		const Knobs pair   = { { "Stages", 0 }, { "Power Stage", 2 }, { "Power Valve", static_cast< float >( pv ) }, { "Drive", slider } };

		const std::vector< float > img = rig.run( grating( W, H, a, cycles ), pair );
		const std::vector< double > y  = rowOf( img, W, H / 2, 1 );
		const double h1 = harmonic( y, cycles, 1 );
		const double r2 = harmonic( y, cycles, 2 ) / h1;
		const double r3 = harmonic( y, cycles, 3 ) / h1;
		//What can make H2 in an odd stage: the index's float rounding is
		//coarser above 2048 than below it (2^-12 against 2^-13), so +d and
		//-d can land a twelfth-bit apart; times the largest node step on the
		//way; and the output's own rounding.
		const vchain::Table& table = rig.tables().Row( vchain::RowIndex( vchain::kMain, 1, vchain::kPower ) );
		double maxStep = 0.0;
		for( int i = 0; i + 1 < vchain::kNodes; ++i )
			if( std::fabs( table.warp.At( i ) ) <= drive * a * 1.01 )
				maxStep = std::max( maxStep, std::fabs( static_cast< double >( table.values[ static_cast< size_t >( i + 1 ) ] ) - table.values[ static_cast< size_t >( i ) ] ) );
		const double floor = 2.0 * ( maxStep / 4096.0 / bplus + 8 * kUlp1 ) / h1;
		failures += report( r2 <= floor, "harmonics EL34 pair  : matched, H2/H1 %.2g, below the float's asymmetry floor %.2g", r2, floor );
		failures += report( r3 > 1e-3 && r3 > 100.0 * floor, "harmonics EL34 pair  : and the odd harmonics stay: H3/H1 %.4f", r3 );

		double h2[ 2 ], model2[ 2 ];
		const double ms[ 2 ] = { 0.02, 0.04 };
		for( int i = 0; i < 2; ++i )
		{
			Knobs k = pair;
			const float mslider = sliderForMismatch( ms[ i ] );
			k.push_back( { "Mismatch", mslider } );
			const std::vector< float > mi = rig.run( grating( W, H, a, cycles ), k );
			const std::vector< double > g = rowOf( mi, W, H / 2, 1 );//green: the valve set at nominal
			h2[ i ] = harmonic( g, cycles, 2 ) / harmonic( g, cycles, 1 );
			const double m = 0.25 * static_cast< double >( mslider );
			const std::vector< double > md = sampled(
				[ & ]( double s ) { return statedPushPull( pv, bias, drive * s, 1.0 + m, 1.0 - m ); }, W, a, cycles );
			model2[ i ] = harmonic( md, cycles, 2 ) / harmonic( md, cycles, 1 );
		}
		const double ratio = h2[ 1 ] / h2[ 0 ];
		const double tol   = std::fabs( model2[ 1 ] / model2[ 0 ] - 2.0 ) + 2.0 * ( floor / h2[ 0 ] + floor / h2[ 1 ] );
		failures += report( std::fabs( ratio - 2.0 ) <= tol,
		                    "harmonics EL34 pair  : mismatch 2%% gives H2/H1 %.5f, 4%% gives %.5f: x%.4f against x2, tolerance %.2g", h2[ 0 ],
		                    h2[ 1 ], ratio, tol );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --crossover
//
// Biased cold, both halves of a push-pull pair are nearly off at rest and
// the stage's gain is the sum of two small transconductances: the slope has
// a notch at the centre, against the slope a little way out where one valve
// has woken. Biasing hotter fills the notch in. At every bias the centre
// slope is the composite load line's small-signal value,
// ( R_aa/4 )( gm1 + gm2 ) / ( 1 + ( R_aa/4 )( 1/r_p1 + 1/r_p2 ) ), and the
// notch's depth -- the centre slope over the lesser slope a twentieth of
// the ramp either side -- is the model's own.
//
// What it does NOT claim: that a hot pair's centre is its steepest point.
// At 450 V an EL34 idling at its full rating sits a few volts above
// Koren's cutoff knee, where gm is still convex, so even there one valve
// driven hard out-steepens the idling pair. "Class A" is a matter of
// degree here, and the notch's depth is the measure of it.
//---------------------------------------------------------------------------
int runCrossover( int W, int H, int perturb )
{
	Rig rig( W, H, perturb );
	if( !rig.ok )
		return report( false, "crossover: could not start the plugin" );
	int failures = 0;
	const int pv = model::kEL34;
	const Stated& v = kStatedPowers[ pv ];
	const StatedPower& c = kStatedCircuits[ pv ];
	const double coldBias = statedPowerBias( pv, 0.05 );
	const float slider    = sliderForDrive( 4.0 * std::fabs( coldBias ) );
	const double drive    = driveOf( slider );
	const int mid = W / 2, out = W / 20;

	double worstCentre = 0.0, worstNotch = 0.0;
	std::vector< double > depths;
	for( const double idle : { 0.05, 0.25, 0.5, 0.75, 1.0 } )
	{
		const float idleSlider = sliderForIdle( idle );
		const double bias = statedPowerBias( pv, 0.05 + 0.95 * static_cast< double >( idleSlider ) );
		const std::vector< float > img = rig.run(
			ramp( W, H ), { { "Stages", 0 }, { "Power Stage", 2 }, { "Power Valve", static_cast< float >( pv ) }, { "Drive", slider },
		                    { "Power Bias", idleSlider } } );
		const std::vector< double > y = rowOf( img, W, H / 2, 1 );
		auto stage = [ & ]( double s ) { return statedPushPull( pv, bias, drive * s ) / c.bplus; };

		//The centre falls between pixels mid-1 and mid: the slope there. A
		//twentieth out, the centred difference about that pixel.
		const double centre = ( y[ static_cast< size_t >( mid ) ] - y[ static_cast< size_t >( mid - 1 ) ] ) * W;
		auto slope  = [ & ]( int x ) { return ( y[ static_cast< size_t >( x + 1 ) ] - y[ static_cast< size_t >( x - 1 ) ] ) * W / 2.0; };
		auto mslope = [ & ]( int x ) { return ( stage( rampAt( x + 1, W ) - 0.5 ) - stage( rampAt( x - 1, W ) - 0.5 ) ) * W / 2.0; };
		const double side  = std::min( slope( mid - out ), slope( mid + out ) );
		const double mside = std::min( mslope( mid - out ), mslope( mid + out ) );
		const double mcentre = ( stage( 0.5 / W ) - stage( -0.5 / W ) ) * W;

		const double h  = 1e-3;
		const double gm = ( koren( v, bias + h, c.bplus, c.bplus ) - koren( v, bias - h, c.bplus, c.bplus ) ) / ( 2 * h );
		const double rp = 2 * h / ( koren( v, bias, c.bplus + h, c.bplus ) - koren( v, bias, c.bplus - h, c.bplus ) );
		const double q4 = c.raa / 4.0;
		const double theory = q4 * 2.0 * gm / ( 1.0 + q4 * 2.0 / rp ) * drive / c.bplus;

		//Each reading is good to the table's interpolation and the float;
		//a difference of two, over its span, is good to twice that.
		const vchain::Tables& t = rig.tables();
		const int row = vchain::RowIndex( vchain::kMain, 1, vchain::kPower );
		const double span = drive * ( out + 2.0 ) / W;
		const double e = interpBound( [ & ]( double d ) { return statedPushPull( pv, bias, d ); }, t.Row( row ).warp, -span, span )
		                 + lookupFloatError( t, row, span, 2 * ulpOf( span ) );
		const double per = 2.0 * ( e / c.bplus + 4 * kUlp1 ) * W;
		const double tolCentre = std::fabs( mcentre - theory ) + per;
		worstCentre = std::max( worstCentre, std::fabs( centre - theory ) / tolCentre );

		const double depth = centre / side, mdepth = mcentre / mside;
		const double tolDepth = depth * ( per / std::fabs( centre ) + per / 2.0 / std::fabs( side ) );
		worstNotch = std::max( worstNotch, std::fabs( depth - mdepth ) / tolDepth );
		depths.push_back( depth );
		note( "crossover idle %3.0f%%: centre slope %.5f (composite load line %.5f), notch depth %.4f (model %.4f)\n", 100.0 * idle, centre, theory,
		      depth, mdepth );
	}
	failures += report( worstCentre <= 1.0, "crossover: at five biases the centre slope is the composite load line's (worst %.2f of tolerance)",
	                    worstCentre );
	failures += report( worstNotch <= 1.0, "crossover: and the notch's depth is the model's (worst %.2f of tolerance)", worstNotch );
	bool fills = true;
	for( size_t i = 1; i < depths.size(); ++i )
		fills = fills && depths[ i ] > depths[ i - 1 ];
	failures += report( depths.front() < 1.0 && fills, "crossover: cold, the centre has %.0f%% of the slope beside it -- a notch -- and biasing hotter fills it (%.0f%% at 100 %%)",
	                    100.0 * depths.front(), 100.0 * depths.back() );
	return failures;
}

//---------------------------------------------------------------------------
// --hue
//
// Y/C carries chroma as a subcarrier, A cos( theta + hue ). Through any
// memoryless chain the output is periodic in theta with the same phase, so
// the fundamental the decoder keeps has the hue it went in with: compressed
// in amplitude by the describing function, F1( A ) = ( 1 / pi ) integral of
// g( A cos t ) cos t dt over a cycle, and never rotated. Twelve hues by eight
// amplitudes at mid-grey: the hue to float precision, the amplitude to the
// model's describing function (4096-point quadrature) within the plugin's
// 64-point rule and its 256-node table, compressing as A grows. The same
// patches through the same valve in RGB move the hue.
//---------------------------------------------------------------------------
struct Patch
{
	double hue, amp;
	double r, g, b;
};

std::vector< float > patches( int W, int H, int hues, int amps, double ampStep, std::vector< Patch >& out )
{
	std::vector< float > p = blank( W, H );
	out.clear();
	for( int j = 0; j < hues; ++j )
		for( int k = 0; k < amps; ++k )
		{
			Patch q;
			q.hue = 2.0 * kPi * j / hues + 0.1;
			q.amp = ampStep * ( k + 1 );
			fromYUV( 0.5, q.amp * std::cos( q.hue ), q.amp * std::sin( q.hue ), q.r, q.g, q.b );
			//What the GPU will actually read: the floats.
			q.r = static_cast< float >( q.r ), q.g = static_cast< float >( q.g ), q.b = static_cast< float >( q.b );
			out.push_back( q );
			for( int y = k * H / amps; y < ( k + 1 ) * H / amps; ++y )
				for( int x = j * W / hues; x < ( j + 1 ) * W / hues; ++x )
					put( p, W, x, y, q.r, q.g, q.b );
		}
	return p;
}

void centreOf( const std::vector< float >& img, int W, int H, int hues, int amps, int j, int k, double& r, double& g, double& b )
{
	const int x = ( j * W / hues + ( j + 1 ) * W / hues ) / 2;
	const int y = ( k * H / amps + ( k + 1 ) * H / amps ) / 2;
	r = at( img, W, y, x, 0 ), g = at( img, W, y, x, 1 ), b = at( img, W, y, x, 2 );
}

double wrap( double a )
{
	while( a > kPi )
		a -= 2 * kPi;
	while( a < -kPi )
		a += 2 * kPi;
	return a;
}

int runHue( int W, int H, int perturb )
{
	Rig rig( W, H, perturb );
	if( !rig.ok )
		return report( false, "hue: could not start the plugin" );
	const int hues = 12, amps = 8;
	const double ampStep = 0.025;
	std::vector< Patch > list;
	const std::vector< float > picture = patches( W, H, hues, amps, ampStep, list );

	const float slider = sliderForDrive( 15.0 );
	const double drive = driveOf( slider );
	const std::vector< float > img = rig.run(
		picture, { { "Signal", 1 }, { "Output", static_cast< float >( vchain::Output::Unity ) }, { "Polarity", 0 }, { "Stages", 0 },
		           { "Power Stage", 0 }, { "Chroma Stages", 1 }, { "Chroma Preamp", model::k12AX7 }, { "Chroma Bias", sliderForBias( 0.5 ) },
		           { "Chroma Drive", slider }, { "Chroma Pwr Stage", 0 } } );

	//The model's describing function, normalised as Unity normalises.
	const Stated& v = kStatedPreamps[ model::k12AX7 ];
	const StatedQ q = statedQuiescent( v, statedCutoff( v, kSupply ) * 0.5 );
	auto g = [ & ]( double s ) { return statedPlate( v, q, q.eg + drive * s, kStopper ); };
	const double slope = derivative( g, 0.0, 1e-6 );
	auto describe = [ & ]( double A, int taps ) {
		double f = 0.0;
		for( int i = 0; i < taps; ++i )
		{
			const double c = std::cos( 2.0 * kPi * i / taps );
			f += g( A * c ) * c;
		}
		return f * 2.0 / taps / slope;
	};
	//The plugin's chroma table is 256 nodes over [ 0, reach ]: the lerp's
	//worst error near A, from the model's own curve.
	const double reach = std::hypot( kKu * 0.299, kKv * 0.701 );
	const double nodeA = reach / 255.0;
	const vchain::Tables& T = rig.tables();
	const int stageRow      = vchain::RowIndex( vchain::kChroma, 1, vchain::kFirst );

	int failures = 0;
	double worstHue = 0.0, worstHueTol = 0.0, worstAmp = 0.0, worstAmpTol = 0.0;
	bool compresses = true;
	for( int j = 0; j < hues; ++j )
	{
		double last = 1e9;
		for( int k = 0; k < amps; ++k )
		{
			const Patch& in = list[ static_cast< size_t >( j * amps + k ) ];
			double r, gg, b, y, u, vv, yi, ui, vi;
			centreOf( img, W, H, hues, amps, j, k, r, gg, b );
			toYUV( r, gg, b, y, u, vv );
			toYUV( in.r, in.g, in.b, yi, ui, vi );
			const double ampOut = std::hypot( u, vv ), ampIn = std::hypot( ui, vi );
			const double dHue   = std::fabs( wrap( std::atan2( vv, u ) - std::atan2( vi, ui ) ) );
			//Each output channel is good to a few dozen float ULPs of 1.0
			//after the chain, the normalisation and two colour conversions;
			//through U = 0.492 ( B - Y ), V = 0.877 ( R - Y ) that is the
			//chroma's error, and over the amplitude, the hue's.
			const double dc   = 64.0 * kUlp1;
			const double hTol = 3.0 * ( kKu * dc + kKv * dc ) / ampOut;
			worstHue    = std::max( worstHue, dHue / hTol );
			worstHueTol = std::max( worstHueTol, hTol );

			const double model  = describe( ampIn, 4096 );
			const double rule   = std::fabs( describe( ampIn, 64 ) - model );
			const double i0     = std::floor( ampIn / nodeA );
			const double lo     = describe( i0 * nodeA, 64 ), hi = describe( ( i0 + 1 ) * nodeA, 64 );
			const double lerp   = 1.25 * std::fabs( lo + ( ampIn / nodeA - i0 ) * ( hi - lo ) - describe( ampIn, 64 ) );
			const double stageE = interpBound( [ & ]( double vg ) { return statedPlate( v, q, vg, kStopper ); }, T.Row( stageRow ).warp,
			                                   q.eg - drive * ampIn, q.eg + drive * ampIn );
			const double aTol   = rule + lerp + 2.0 * stageE / std::fabs( slope ) + 2.0 * dc;
			worstAmp    = std::max( worstAmp, std::fabs( ampOut - model ) / aTol );
			worstAmpTol = std::max( worstAmpTol, aTol );
			if( ampOut / ampIn > last + 1e-6 )
				compresses = false;
			last = ampOut / ampIn;
		}
	}
	failures += report( worstHue <= 1.0, "hue Y/C: 96 patches, the hue kept within the float's bound (worst %.2f of it; bounds to %.2g rad)", worstHue,
	                    worstHueTol );
	failures += report( worstAmp <= 1.0, "hue Y/C: the chroma amplitude follows the describing function (worst %.2f of its tolerance, up to %.2g)",
	                    worstAmp, worstAmpTol );
	failures += report( compresses, "hue Y/C: A'/A falls as A rises in every hue -- the chain compresses colour, it does not turn it" );

	//--- RGB through the same valve moves the hue.
	const std::vector< float > rgb = rig.run(
		picture, { { "Signal", 0 }, { "Chroma Stages", 0 }, { "Stages", 1 }, { "Preamp", model::k12AX7 }, { "Drive", slider }, { "Bias", sliderForBias( 0.5 ) } } );
	double moved = 0.0;
	for( int j = 0; j < hues; ++j )
		for( int k = 0; k < amps; ++k )
		{
			const Patch& in = list[ static_cast< size_t >( j * amps + k ) ];
			double r, gg, b, y, u, vv, yi, ui, vi;
			centreOf( rgb, W, H, hues, amps, j, k, r, gg, b );
			toYUV( r, gg, b, y, u, vv );
			toYUV( in.r, in.g, in.b, yi, ui, vi );
			if( std::hypot( u, vv ) > 1e-3 )
				moved = std::max( moved, std::fabs( wrap( std::atan2( vv, u ) - std::atan2( vi, ui ) ) ) );
		}
	failures += report( moved > 0.05 && moved > 100.0 * worstHueTol, "hue RGB: the same valve on each channel turns hue by up to %.3f rad", moved );
	return failures;
}

//---------------------------------------------------------------------------
// --dg
//
// Composite: the subcarrier rides the luma through ONE chain. To first
// order its gain is the chain's slope at that luma, f1( Y ): a modulated
// staircase -- seven steps of luma with the same small subcarrier on each --
// comes out with its chroma in proportion to f1. That is differential gain,
// read the way a vectorscope's DG test reads it. To second order the
// curvature rectifies part of the carrier into the mean: a strong colour
// moves its own luma by f2 a^2 / 4. In Y/C the chroma has its own chain and
// the staircase leaves it alone.
//---------------------------------------------------------------------------
int runDg( int W, int H, int perturb )
{
	Rig rig( W, H, perturb );
	if( !rig.ok )
		return report( false, "dg: could not start the plugin" );
	const int steps    = 7;
	const double small = 0.01, hue = 0.7;
	const float slider = sliderForDrive( 4.0 );
	const double drive = driveOf( slider );

	auto staircase = [ & ]( double a ) {
		std::vector< float > p = blank( W, H );
		for( int i = 0; i < steps; ++i )
		{
			const double level = 0.2 + 0.1 * i;
			double r, g, b, r0, g0, b0;
			fromYUV( level, a * std::cos( hue ), a * std::sin( hue ), r, g, b );
			fromYUV( level, 0.0, 0.0, r0, g0, b0 );
			for( int y = i * H / steps; y < ( i + 1 ) * H / steps; ++y )
				for( int x = 0; x < W; ++x )
				{
					if( x < W / 2 )
						put( p, W, x, y, r, g, b );
					else
						put( p, W, x, y, r0, g0, b0 );
				}
		}
		return p;
	};
	auto readStep = [ & ]( const std::vector< float >& img, int i, bool carrier, double& y, double& amp ) {
		const int row = ( i * H / steps + ( i + 1 ) * H / steps ) / 2;
		const int col = carrier ? W / 4 : 3 * W / 4;
		double u, v;
		toYUV( at( img, W, row, col, 0 ), at( img, W, row, col, 1 ), at( img, W, row, col, 2 ), y, u, v );
		amp = std::hypot( u, v );
	};

	const Knobs composite = { { "Signal", 2 }, { "Output", static_cast< float >( vchain::Output::Unity ) }, { "Polarity", 0 }, { "Stages", 1 },
		                      { "Preamp", model::k12AX7 }, { "Power Stage", 0 }, { "Drive", slider }, { "Bias", sliderForBias( 0.5 ) } };
	const Stated& v = kStatedPreamps[ model::k12AX7 ];
	const StatedQ q = statedQuiescent( v, statedCutoff( v, kSupply ) * 0.5 );
	auto f = [ & ]( double s ) { return statedPlate( v, q, q.eg + drive * s, kStopper ); };
	const double h = 2e-3;
	auto f1 = [ & ]( double s ) { return derivative( f, s, h ); };
	auto f2 = [ & ]( double s ) { return ( f( s + h ) - 2 * f( s ) + f( s - h ) ) / ( h * h ); };
	auto f3 = [ & ]( double s ) { return ( f( s + 2 * h ) - 2 * f( s + h ) + 2 * f( s - h ) - f( s - 2 * h ) ) / ( 2 * h * h * h ); };
	const double unity = f1( 0.0 );
	//The composite table's first A node, the reach over 63.
	const double A1 = std::hypot( kKu * 0.299, kKv * 0.701 ) / 63.0;
	//Taylor: F1 = a f1 + a^3 f3 / 8, so the measured ratio is good to
	//a^2 |f3| / ( 8 |f1| ) at each end; the table's bilinear step in Y
	//(1/255) costs h^2 a |f3| / 8, and its first A node a ( A1^2 - a^2 ) |f3| / 8.
	auto relative = [ & ]( double s ) {
		return ( small * small + std::pow( 1.0 / 255.0, 2 ) + ( A1 * A1 - small * small ) ) * std::fabs( f3( s ) ) / ( 8.0 * std::fabs( f1( s ) ) );
	};

	int failures = 0;
	{
		const std::vector< float > img = rig.run( staircase( small ), composite );
		double yRef, aRef;
		readStep( img, 3, true, yRef, aRef );
		double worst = 0.0, lo = 1e9, hi = 0.0;
		const vchain::Tables& t = rig.tables();
		const int row = vchain::RowIndex( vchain::kMain, 1, vchain::kFirst );
		for( int i = 0; i < steps; ++i )
		{
			const double s = 0.1 * ( i - 3 );
			double y, amp;
			readStep( img, i, true, y, amp );
			const double measured = amp / aRef;
			const double expected = f1( s ) / f1( 0.0 );
			//The stage table's interpolation error e (volts) at each of the
			//64 taps moves the fundamental by at most 2e, against a
			//fundamental of a |f1|: relatively 2e / ( a |f1| ), at this step
			//and at the reference.
			const auto table = [ & ]( double ss ) {
				return interpBound( [ & ]( double vg ) { return statedPlate( v, q, vg, kStopper ); }, t.Row( row ).warp,
				                    q.eg + drive * ( ss - A1 ), q.eg + drive * ( ss + A1 ) );
			};
			const double tables = 2.0 * table( s ) / ( small * std::fabs( f1( s ) ) ) + 2.0 * table( 0.0 ) / ( small * std::fabs( f1( 0.0 ) ) );
			const double fl     = 64.0 * kUlp1 * ( 1.0 / amp + 1.0 / aRef ) * measured;
			const double tol    = expected * ( relative( s ) + relative( 0.0 ) ) + fl + measured * tables;
			worst = std::max( worst, std::fabs( measured - expected ) / tol );
			lo    = std::min( lo, measured );
			hi    = std::max( hi, measured );
		}
		failures += report( worst <= 1.0, "dg Composite: chroma gain at seven luma steps follows f1( Y ) / f1( 0.5 ) (worst %.2f of tolerance)", worst );
		failures += report( hi / lo > 1.2, "dg Composite: and it is not flat -- differential gain of %.0f%% across the staircase", 100.0 * ( hi - lo ) / hi );
	}
	{
		const double big = 0.2;
		const std::vector< float > img = rig.run( staircase( big ), composite );
		double yWith, yWithout, a;
		readStep( img, 3, true, yWith, a );
		readStep( img, 3, false, yWithout, a );
		const double measured = yWith - yWithout;
		const double taylor   = f2( 0.0 ) * big * big / 4.0 / unity;
		double exact = 0.0;
		for( int i = 0; i < 4096; ++i )
			exact += f( big * std::cos( 2.0 * kPi * i / 4096 ) );
		exact = ( exact / 4096 - f( 0.0 ) ) / unity;
		//The remainder of Taylor, the model's own; the bilinear table in A
		//(nodes 0.01 apart) with the model's curvature in A, f2 / 2, over
		//them; the stage table; the float.
		const vchain::Tables& t = rig.tables();
		const int row = vchain::RowIndex( vchain::kMain, 1, vchain::kFirst );
		const double table = interpBound( [ & ]( double vg ) { return statedPlate( v, q, vg, kStopper ); }, t.Row( row ).warp, q.eg - drive * big,
		                                  q.eg + drive * big ) / std::fabs( unity );
		const double nodes = std::pow( A1, 2 ) / 8.0 * std::fabs( f2( 0.0 ) ) / 2.0 / std::fabs( unity ) * 2.0;
		const double tol   = std::fabs( exact - taylor ) + nodes + 2.0 * table + 128.0 * kUlp1;
		failures += report( std::fabs( measured - taylor ) <= tol && std::fabs( exact - taylor ) < 0.5 * std::fabs( taylor ),
		                    "dg Composite: a carrier of 0.2 moves its luma by %+.5f against f2 a^2 / 4 = %+.5f, tolerance %.2g", measured, taylor, tol );
	}
	{
		Knobs yc = composite;
		for( const auto& k : Knobs { { "Signal", 1 }, { "Chroma Stages", 1 }, { "Chroma Preamp", model::k12AX7 }, { "Chroma Drive", slider },
		                             { "Chroma Bias", sliderForBias( 0.5 ) }, { "Chroma Pwr Stage", 0 } } )
			yc.push_back( k );
		const std::vector< float > img = rig.run( staircase( small ), yc );
		double y, aRef, worst = 0.0;
		readStep( img, 3, true, y, aRef );
		for( int i = 0; i < steps; ++i )
		{
			double amp;
			readStep( img, i, true, y, amp );
			worst = std::max( worst, std::fabs( amp / aRef - 1.0 ) );
		}
		//Only the colour conversions differ from step to step: each output
		//channel good to 16 float ULPs of 1.0, over the chroma amplitude.
		const double bound = 2.0 * 16.0 * kUlp1 / aRef;
		failures += report( worst <= bound, "dg Y/C: the chroma's gain does not depend on the luma (worst %.2g, bound %.2g)", worst, bound );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --polarity
//---------------------------------------------------------------------------
int runPolarity( int W, int H, int perturb )
{
	Rig rig( W, H, perturb );
	if( !rig.ok )
		return report( false, "polarity: could not start the plugin" );
	struct Case
	{
		int stages, power, sign;
	};
	//Every common-cathode stage inverts; a single-ended power stage is one
	//more; the push-pull pair, read across its primary, is not.
	const Case cases[] = { { 0, 0, +1 }, { 1, 0, -1 }, { 2, 0, +1 }, { 3, 0, -1 }, { 4, 0, +1 },
		                   { 0, 1, -1 }, { 0, 2, +1 }, { 1, 1, +1 }, { 1, 2, -1 }, { 2, 1, -1 } };
	const int count = static_cast< int >( sizeof( cases ) / sizeof( cases[ 0 ] ) );
	int failures = 0, wrong = 0, wrongCorrected = 0;
	for( const Case& c : cases )
		for( int polarity = 0; polarity < 2; ++polarity )
		{
			const std::vector< float > img = rig.run(
				ramp( W, H ), { { "Output", 0 }, { "Polarity", static_cast< float >( polarity ) }, { "Stages", static_cast< float >( c.stages ) },
				                { "Power Stage", static_cast< float >( c.power ) }, { "Drive", sliderForDrive( 0.5 ) } } );
			const double rise = at( img, W, H / 2, W - 1, 1 ) - at( img, W, H / 2, 0, 1 );
			const int sign    = rise > 0 ? 1 : -1;
			if( polarity == 1 && sign != c.sign )
				++wrong;
			if( polarity == 0 && sign != 1 )
				++wrongCorrected;
		}
	failures += report( wrong == 0, "polarity: as wired, %d of %d chains have the sign their inversions give", count - wrong, count );
	failures += report( wrongCorrected == 0, "polarity: corrected, every chain rises from black to white (%d do not)", wrongCorrected );

	//An inverted chroma chain turns every hue half way round.
	std::vector< Patch > list;
	const std::vector< float > picture = patches( W, H, 6, 1, 0.15, list );
	for( int polarity = 0; polarity < 2; ++polarity )
	{
		const std::vector< float > img = rig.run(
			picture, { { "Signal", 1 }, { "Polarity", static_cast< float >( polarity ) }, { "Stages", 0 }, { "Power Stage", 0 }, { "Chroma Stages", 1 },
			           { "Chroma Drive", sliderForDrive( 1.0 ) }, { "Chroma Pwr Stage", 0 }, { "Output", 0 } } );
		double worst = 0.0;
		for( int j = 0; j < 6; ++j )
		{
			double r, g, b, y, u, v, yi, ui, vi;
			centreOf( img, W, H, 6, 1, j, 0, r, g, b );
			toYUV( r, g, b, y, u, v );
			const Patch& in = list[ static_cast< size_t >( j ) ];
			toYUV( in.r, in.g, in.b, yi, ui, vi );
			worst = std::max( worst, std::fabs( wrap( std::atan2( v, u ) - std::atan2( vi, ui ) - ( polarity == 1 ? kPi : 0.0 ) ) ) );
		}
		failures += report( worst < 1e-4, "polarity: Y/C, one chroma stage %s: every hue %s (worst %.2g rad off)", polarity ? "as wired" : "corrected",
		                    polarity ? "turned by pi" : "kept", worst );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --identity
//---------------------------------------------------------------------------
int runIdentity( int W, int H, int perturb )
{
	Rig rig( W, H, perturb );
	if( !rig.ok )
		return report( false, "identity: could not start the plugin" );
	int failures = 0;
	const std::vector< float > picture = noise( W, H, 7 );

	//No valves: a wire, in every mode and both normalisations. The bound:
	//the colour round trip and, in Y/C and Composite, a 64-term sum of
	//floats whose cosines are themselves floats -- a few dozen ULPs.
	for( int signal = 0; signal < 3; ++signal )
		for( int output = 0; output < 2; ++output )
		{
			const std::vector< float > img = rig.run(
				picture, { { "Signal", static_cast< float >( signal ) }, { "Output", static_cast< float >( output ) }, { "Polarity", 0 }, { "Stages", 0 },
				           { "Power Stage", 0 }, { "Chroma Stages", 0 }, { "Chroma Pwr Stage", 0 } } );
			double worst = 0.0;
			for( size_t i = 0; i < img.size(); ++i )
				worst = std::max( worst, static_cast< double >( std::fabs( img[ i ] - picture[ i ] ) ) );
			const double bound = signal == 0 ? 4.0 * kUlp1 : 96.0 * kUlp1;
			failures += report( worst <= bound, "identity: no valves, %-9s %-5s: a wire (worst %.2g, bound %.2g)",
			                    signal == 0 ? "RGB," : signal == 1 ? "Y/C," : "Composite,", output == 0 ? "Fit" : "Unity", worst, bound );
		}

	//A whisper of drive, Unity: the curve's departure from its tangent is
	//the model's own, bounded over the whole ramp.
	{
		const float slider = sliderForDrive( 0.01 );
		const double drive = driveOf( slider );
		const std::vector< float > img =
			rig.run( ramp( W, H ), { { "Signal", 0 }, { "Output", 1 }, { "Polarity", 0 }, { "Stages", 1 }, { "Preamp", model::k12AU7 }, { "Drive", slider },
			                         { "Power Stage", 0 } } );
		const Stated& v = kStatedPreamps[ model::k12AU7 ];
		const StatedQ q = statedQuiescent( v, statedCutoff( v, kSupply ) * 0.5 );
		auto f = [ & ]( double s ) { return statedPlate( v, q, q.eg + drive * s, kStopper ); };
		const double slope = derivative( f, 0.0, 1e-6 );
		double departure = 0.0, worst = 0.0;
		for( int x = 0; x < W; ++x )
		{
			const double s = rampAt( x, W ) - 0.5;
			departure      = std::max( departure, std::fabs( f( s ) / slope - s ) );
			worst          = std::max( worst, std::fabs( at( img, W, H / 2, x, 1 ) - rampAt( x, W ) ) );
		}
		//The tables' interpolation and the float at this drive: far below
		//the curve's own departure, which is what is being shown.
		const double bound = departure * 1.01 + 2e-6;
		failures += report( worst <= bound && bound < 1e-3, "identity: 10 mV on a 12AU7, Unity: the input to %.2g (the curve's own departure %.2g)", worst,
		                    departure );
	}

	//Mix 0 is the input, bit for bit; alpha passes through; premultiplied
	//colour is un-multiplied, shaped and multiplied back.
	{
		const std::vector< float > img = rig.run( picture, { { "Signal", 2 }, { "Stages", 2 }, { "Power Stage", 2 }, { "Mix", 0 } } );
		failures += report( img == picture, "identity: Mix 0 returns the input bit for bit" );
	}
	{
		std::vector< float > half = picture;
		for( size_t i = 0; i < half.size(); i += 4 )
		{
			half[ i + 0 ] *= 0.5f;
			half[ i + 1 ] *= 0.5f;
			half[ i + 2 ] *= 0.5f;
			half[ i + 3 ] = 0.5f;
		}
		const Knobs k = { { "Signal", 0 }, { "Mix", 1 }, { "Output", 0 }, { "Stages", 2 }, { "Power Stage", 2 }, { "Drive", sliderForDrive( 1.0 ) } };
		const std::vector< float > opaque = rig.run( picture, k );
		const std::vector< float > img    = rig.run( half, k );
		double worst = 0.0;
		bool alpha   = true;
		for( size_t i = 0; i < img.size(); i += 4 )
		{
			alpha = alpha && img[ i + 3 ] == 0.5f;
			for( int c = 0; c < 3; ++c )
				worst = std::max( worst, static_cast< double >( std::fabs( img[ i + c ] - 0.5f * opaque[ i + c ] ) ) );
		}
		failures += report( alpha && worst <= 4.0 * kUlp1, "identity: half-transparent premultiplied input keeps its alpha and is the opaque result halved (worst %.2g)",
		                    worst );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --reference
//
// Two comparisons. The picture against the CPU running the plugin's own
// tables through the shader's arithmetic in double (Tables::Evaluate, the
// same normalisation): this proves the GPU runs the chain it was given, to
// a bound walked lookup by lookup from the float's known error. And, in
// RGB, the CPU's run of the tables against the harness's own model of the
// valves -- its own Koren, its own operating points, its own solves --
// to the tables' interpolation bound carried through the downstream slopes:
// this proves the tables are the valves.
//---------------------------------------------------------------------------
struct RefSetting
{
	const char* what;
	Knobs knobs;
};

std::vector< RefSetting > referenceSettings()
{
	return {
		{ "defaults (2 x 12AX7, EL34 pair)", { { "Output", 0 }, { "Polarity", 0 }, { "Stages", 2 }, { "Power Stage", 2 }, { "Drive", sliderForDrive( 1.0 ) },
		                                         { "Master", sliderForMaster( 0.3 ) } } },
		{ "4 x 12AT7 hot, KT88 SE, Unity", { { "Output", 1 }, { "Polarity", 0 }, { "Stages", 4 }, { "Preamp", 1 }, { "Bias", 0.9f }, { "Power Stage", 1 },
		                                     { "Power Valve", 2 }, { "Drive", sliderForDrive( 0.3 ) }, { "Master", sliderForMaster( 0.05 ) } } },
		{ "6DJ8 cold, 300B pair, as wired, rest 0.2", { { "Output", 0 }, { "Polarity", 1 }, { "Rest Level", 0.2f }, { "Stages", 1 }, { "Preamp", 3 }, { "Bias", 0.1f }, { "Power Stage", 2 },
		                                      { "Power Valve", 3 }, { "Drive", sliderForDrive( 8.0 ) }, { "Master", sliderForMaster( 2.0 ) } } },
		{ "mismatch 20 %, 6L6GC pair, Plate", { { "Output", 2 }, { "Polarity", 0 }, { "Stages", 2 }, { "Mismatch", sliderForMismatch( 0.2 ) }, { "Power Stage", 2 },
		                                        { "Power Valve", 1 }, { "Drive", sliderForDrive( 2.0 ) }, { "Master", sliderForMaster( 0.2 ) } } },
		{ "Y/C: 12AX7 luma rest 0.7, 2 x 12AU7 chroma", { { "Signal", 1 }, { "Output", 0 }, { "Polarity", 0 }, { "Rest Level", 0.7f }, { "Stages", 1 }, { "Drive", sliderForDrive( 2.0 ) },
		                                         { "Power Stage", 0 }, { "Chroma Stages", 2 }, { "Chroma Preamp", 2 }, { "Chroma Drive", sliderForDrive( 20.0 ) },
		                                         { "Chroma Pwr Stage", 0 } } },
		{ "Composite: 12AX7, EL34 pair, rest 0.3", { { "Signal", 2 }, { "Output", 1 }, { "Polarity", 0 }, { "Rest Level", 0.3f }, { "Stages", 1 }, { "Drive", sliderForDrive( 3.0 ) },
		                                          { "Power Stage", 2 }, { "Master", sliderForMaster( 0.1 ) } } },
	};
}

struct Emulated
{
	double r, g, b;
	double bound;///< against the GPU
};

struct DescribedTables
{
	std::vector< std::pair< double, double > > comp;///< 256 x 64: ( mean, fundamental )
	std::vector< double > chroma;                   ///< 256
	std::vector< double > compErr, chromaErr;       ///< what the float can do to each node
};

/// The display pass for one pixel, from the plugin's own tables, in double.
Emulated emulate( const vchain::Tables& t, const vchain::Norm& n, int signal, const float* px, const DescribedTables& d )
{
	Emulated e {};
	const double x[ 3 ] = { px[ 0 ], px[ 1 ], px[ 2 ] };
	//The shader is handed Rest as a float.
	const double rest = static_cast< float >( t.Current().rest );
	if( signal == 0 )
	{
		double out[ 3 ], worst = 0.0;
		for( int c = 0; c < 3; ++c )
		{
			const size_t cc = static_cast< size_t >( c );
			out[ c ] = n.offset + ( t.Evaluate( vchain::kMain, c, x[ c ] - rest ) - n.ref[ cc ] ) * n.scale[ cc ];
			worst    = std::max( worst, std::fabs( n.scale[ cc ] ) * chainFloatError( t, vchain::kMain, c, x[ c ] - rest ) );
		}
		e.r = out[ 0 ], e.g = out[ 1 ], e.b = out[ 2 ];
		e.bound = worst + 8.0 * kUlp1;
	}
	else
	{
		const double level = 0.299 * x[ 0 ] + 0.587 * x[ 1 ] + 0.114 * x[ 2 ];
		const double u = 0.492111 * ( x[ 2 ] - level ), v = 0.877283 * ( x[ 0 ] - level );
		const double amp   = std::hypot( u, v );
		const double reach = vchain::ChromaReach();
		double y, a2, dy, da;
		if( signal == 1 )
		{
			y  = n.offset + ( t.Evaluate( vchain::kMain, 1, level - rest ) - n.ref[ 1 ] ) * n.scale[ 1 ];
			dy = std::fabs( n.scale[ 1 ] ) * chainFloatError( t, vchain::kMain, 1, level - rest );
			const double f = std::clamp( amp / reach, 0.0, 1.0 ) * 255.0;
			const int i    = std::min( static_cast< int >( f ), 254 );
			const size_t s = static_cast< size_t >( i );
			a2 = ( d.chroma[ s ] + ( f - i ) * ( d.chroma[ s + 1 ] - d.chroma[ s ] ) ) * n.chromaScale;
			da = std::fabs( n.chromaScale ) * std::max( d.chromaErr[ s ], d.chromaErr[ s + 1 ] );
		}
		else
		{
			const double fy = std::clamp( level, 0.0, 1.0 ) * 255.0, fa = std::clamp( amp / reach, 0.0, 1.0 ) * 63.0;
			const int iy = std::min( static_cast< int >( fy ), 254 ), ia = std::min( static_cast< int >( fa ), 62 );
			const double ty = fy - iy, ta = fa - ia;
			auto node = [ & ]( int yy, int aa ) { return d.comp[ static_cast< size_t >( aa * 256 + yy ) ]; };
			auto err  = [ & ]( int yy, int aa ) { return d.compErr[ static_cast< size_t >( aa * 256 + yy ) ]; };
			const double b0f = node( iy, ia ).first + ty * ( node( iy + 1, ia ).first - node( iy, ia ).first );
			const double b1f = node( iy, ia + 1 ).first + ty * ( node( iy + 1, ia + 1 ).first - node( iy, ia + 1 ).first );
			const double b0s = node( iy, ia ).second + ty * ( node( iy + 1, ia ).second - node( iy, ia ).second );
			const double b1s = node( iy, ia + 1 ).second + ty * ( node( iy + 1, ia + 1 ).second - node( iy, ia + 1 ).second );
			const double dc = b0f + ta * ( b1f - b0f ), fund = b0s + ta * ( b1s - b0s );
			const double ne = std::max( { err( iy, ia ), err( iy + 1, ia ), err( iy, ia + 1 ), err( iy + 1, ia + 1 ) } );
			y  = n.offset + ( dc - n.ref[ 1 ] ) * n.scale[ 1 ];
			a2 = fund * n.chromaScale;
			dy = std::fabs( n.scale[ 1 ] ) * ne;
			da = std::fabs( n.chromaScale ) * ne * 2.0;
		}
		const double cu = amp > 0.0 ? u * a2 / amp : 0.0, cv = amp > 0.0 ? v * a2 / amp : 0.0;
		e.b = y + cu / 0.492111;
		e.r = y + cv / 0.877283;
		e.g = ( y - 0.299 * e.r - 0.114 * e.b ) / 0.587;
		//The colour conversion's worst gain from an error in Y and in A.
		const double dR = dy + da / 0.877283, dB = dy + da / 0.492111;
		e.bound = ( dy + 0.299 * dR + 0.114 * dB ) / 0.587 + dR + dB + 64.0 * kUlp1;
	}
	e.r = std::clamp( e.r, 0.0, 1.0 ), e.g = std::clamp( e.g, 0.0, 1.0 ), e.b = std::clamp( e.b, 0.0, 1.0 );
	return e;
}

DescribedTables describe( const vchain::Tables& t, int signal )
{
	DescribedTables d;
	d.comp.assign( 256 * 64, { 0.0, 0.0 } );
	d.compErr.assign( 256 * 64, 0.0 );
	d.chroma.assign( 256, 0.0 );
	d.chromaErr.assign( 256, 0.0 );
	const double reach  = vchain::ChromaReach();
	const auto& cosines = vchain::CosTable();
	const double rest   = static_cast< float >( t.Current().rest );
	//What the float can do to a node: each of 64 lookups off by its walked
	//error, and a 64-term sum's rounding.
	auto tapError = [ & ]( int chain, double offset, double a ) {
		double worst = 0.0, biggest = 0.0;
		for( int k = 0; k < vchain::kTaps; ++k )
		{
			const double s = offset + a * cosines[ static_cast< size_t >( k ) ];
			worst   = std::max( worst, chainFloatError( t, chain, 1, s ) );
			biggest = std::max( biggest, std::fabs( t.Evaluate( chain, 1, s ) ) );
		}
		return 2.0 * worst + 128.0 * kUlp1 * biggest;
	};
	if( signal == 2 )
		for( int aa = 0; aa < 64; ++aa )
			for( int yy = 0; yy < 256; ++yy )
			{
				double dc = 0.0;
				const double fund = t.Fundamental( vchain::kMain, 1, yy / 255.0 - rest, aa / 63.0 * reach, &dc );
				d.comp[ static_cast< size_t >( aa * 256 + yy ) ]    = { dc, fund };
				d.compErr[ static_cast< size_t >( aa * 256 + yy ) ] = tapError( vchain::kMain, yy / 255.0 - rest, aa / 63.0 * reach );
			}
	if( signal == 1 )
		for( int i = 0; i < 256; ++i )
		{
			d.chroma[ static_cast< size_t >( i ) ]    = t.Fundamental( vchain::kChroma, 1, 0.0, i / 255.0 * reach );
			d.chromaErr[ static_cast< size_t >( i ) ] = tapError( vchain::kChroma, 0.0, i / 255.0 * reach );
		}
	return d;
}

/// The harness's own valves through the chain the plugin was set to, with
/// the bound on how far the plugin's tables may sit from them: each table's
/// interpolation error at the point used, carried through every later
/// stage's slope.
double modelThrough( const vchain::Tables& t, int c, double sgl, double& bound )
{
	const vchain::Settings& s = t.Current();
	const bool split          = s.perChannel;
	const double scale        = split ? 1.0 + s.mismatch * ( 1 - c ) : 1.0;
	const ModelChain mc       = modelOf( s.chain[ 0 ], s.interstage, scale );
	const int base            = t.RowBase( vchain::kMain, c );
	const Stated& v           = *mc.valve;
	const double rout         = 1.0 / ( 1.0 / mc.q.rp + 1.0 / acLoad() );
	bound = 0.0;
	auto stage = [ & ]( double open, double source, int row ) {
		auto P = [ & ]( double vg ) { return statedPlate( v, mc.q, vg, source, true, scale ); };
		bound  = bound * std::fabs( derivative( P, open, 1e-4 ) ) + interpBound( P, t.Row( row ).warp, open, open );
		return P( open );
	};
	double out = 0.0, d = 0.0;
	if( mc.stages > 0 )
	{
		out = stage( mc.q.eg + mc.drive * sgl, kStopper, base + vchain::kFirst );
		for( int k = 1; k < mc.stages; ++k )
		{
			bound *= mc.interstage;
			out = stage( mc.q.eg + mc.interstage * out, rout, base + vchain::kLater );
		}
		if( mc.power == 0 )
			return out;
		d = mc.master * out;
		bound *= mc.master;
	}
	else
	{
		if( mc.power == 0 )
			return mc.drive * sgl;
		d = mc.drive * sgl;
	}
	const double sA = mc.power == 2 ? scale * ( 1.0 + s.mismatch ) : scale;
	const double sB = mc.power == 2 ? scale * ( 1.0 - s.mismatch ) : scale;
	auto Pp = [ & ]( double dd ) {
		return mc.power == 1 ? statedSingleEnded( mc.powerValve, mc.powerBias, dd ) : statedPushPull( mc.powerValve, mc.powerBias, dd, sA, sB );
	};
	bound = bound * std::fabs( derivative( Pp, d, 1e-3 ) ) + interpBound( Pp, t.Row( base + vchain::kPower ).warp, d, d );
	return Pp( d );
}

int runReference( int W, int H, int perturb )
{
	Rig rig( W, H, perturb );
	if( !rig.ok )
		return report( false, "reference: could not start the plugin" );
	int failures = 0;
	const std::vector< float > picture = noise( W, H, 11 );
	for( const RefSetting& setting : referenceSettings() )
	{
		quiet( rig.s.plugin );
		const std::vector< float > img = rig.run( picture, setting.knobs );
		const vchain::Tables& t        = rig.tables();
		const vchain::Norm& n          = rig.s.plugin.NormForTest();
		int signal = 0;
		for( const auto& k : setting.knobs )
			if( std::string( k.first ) == "Signal" )
				signal = static_cast< int >( k.second );
		const DescribedTables d = describe( t, signal );

		double worst = 0.0, worstBound = 0.0;
		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
			{
				const float* px  = picture.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
				const Emulated e = emulate( t, n, signal, px, d );
				const double got[ 3 ]  = { at( img, W, y, x, 0 ), at( img, W, y, x, 1 ), at( img, W, y, x, 2 ) };
				const double want[ 3 ] = { e.r, e.g, e.b };
				for( int c = 0; c < 3; ++c )
				{
					const double ratio = std::fabs( got[ c ] - want[ c ] ) / e.bound;
					if( ratio > worst )
					{
						worst      = ratio;
						worstBound = e.bound;
					}
				}
			}
		failures += report( worst <= 1.0, "reference %-42s: the GPU against the tables in double, worst %.3f of its bound (%.2g)", setting.what, worst,
		                    worstBound );

		//The tables against the valves, RGB only: subsampled, because each
		//pixel is a handful of bisection solves.
		if( signal != 0 )
			continue;
		double worstModel = 0.0, worstModelBound = 0.0;
		const int stride = std::max( 1, W * H / 600 );
		for( int i = 0; i < W * H; i += stride )
		{
			const float* px = picture.data() + static_cast< size_t >( i ) * 4;
			for( int c = 0; c < 3; ++c )
			{
				const double sgl = px[ c ] - static_cast< float >( t.Current().rest );
				double bound     = 0.0;
				const double m   = modelThrough( t, c, sgl, bound );
				const double tol = bound + 1e-7 * std::max( 1.0, std::fabs( m ) );
				const double ratio = std::fabs( t.Evaluate( vchain::kMain, c, sgl ) - m ) / tol;
				if( ratio > worstModel )
				{
					worstModel      = ratio;
					worstModelBound = tol;
				}
			}
		}
		failures += report( worstModel <= 1.0, "reference %-42s: the tables against the valves, worst %.3f of the interpolation bound (%.2g V)",
		                    setting.what, worstModel, worstModelBound );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --curve
//
// Show Curve's box: in the corner the README gives (a side of 32 % of the
// shorter dimension, 3 % in from the bottom-left), each column lights the
// rows between the curve at its left and right edges, 0.8 px either side;
// and not one pixel outside the box changes.
//---------------------------------------------------------------------------
int runCurve( int W, int H, int perturb )
{
	Rig rig( W, H, perturb );
	if( !rig.ok )
		return report( false, "curve: could not start the plugin" );
	const std::vector< float > picture = ramp( W, H );
	const Knobs k = { { "Signal", 0 }, { "Output", 0 }, { "Polarity", 0 }, { "Stages", 2 }, { "Power Stage", 2 }, { "Drive", sliderForDrive( 1.0 ) },
		              { "Master", sliderForMaster( 0.3 ) }, { "Show Curve", 0 } };
	const std::vector< float > off = rig.run( picture, k );
	const std::vector< float > on  = rig.run( picture, { { "Show Curve", 1 } } );
	const vchain::Tables& t = rig.tables();
	const vchain::Norm& n   = rig.s.plugin.NormForTest();
	const int small = std::min( W, H ), side = small * 32 / 100, margin = small * 3 / 100;

	bool outside = true;
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const int bx = x - margin, by = ( H - 1 - y ) - margin;
			if( bx >= 0 && by >= 0 && bx < side && by < side )
				continue;
			for( int c = 0; c < 4; ++c )
				outside = outside && at( on, W, y, x, c ) == at( off, W, y, x, c );
		}

	auto curve = [ & ]( double t01 ) {
		return std::clamp( n.offset + ( t.Evaluate( vchain::kMain, 1, t01 - t.Current().rest ) - n.ref[ 1 ] ) * n.scale[ 1 ], -0.05, 1.05 ) * side;
	};
	int worst = 0, columns = 0;
	for( int bx = 2; bx < side - 2; ++bx )
	{
		const double yl = curve( static_cast< double >( bx ) / side ), yr = curve( static_cast< double >( bx + 1 ) / side );
		const int lo = static_cast< int >( std::ceil( std::min( yl, yr ) - 0.8 - 0.5 ) );
		const int hi = static_cast< int >( std::floor( std::max( yl, yr ) + 0.8 - 0.5 ) );
		//The curve is drawn over the box's border, so a crushed black sits
		//on row 0 and is still the curve.
		int litLo = side, litHi = -1;
		for( int by = 0; by < side; ++by )
		{
			const int y = H - 1 - ( margin + by ), x = margin + bx;
			if( at( on, W, y, x, 0 ) > 0.99f && at( on, W, y, x, 1 ) > 0.99f && at( on, W, y, x, 2 ) > 0.99f )
			{
				litLo = std::min( litLo, by );
				litHi = std::max( litHi, by );
			}
		}
		const int clo = std::clamp( lo, 0, side - 1 ), chi = std::clamp( hi, 0, side - 1 );
		++columns;
		worst = std::max( { worst, std::abs( litLo - clo ), std::abs( litHi - chi ) } );
	}
	int failures = 0;
	failures += report( outside, "curve: Show Curve changes nothing outside its box" );
	failures += report( worst <= 1 && columns > side / 2, "curve: in %d columns the lit span is the curve's, to within %d px", columns, worst );
	return failures;
}

//---------------------------------------------------------------------------
// --model (no GL)
//---------------------------------------------------------------------------
int runModel( int perturb )
{
	int failures = 0;

	//--- The valves are Koren's.
	{
		int bad = 0;
		for( int p = 0; p < model::kPreampCount; ++p )
		{
			const model::Valve& a = model::PreampValve( p, perturb );
			const Stated& b       = kStatedPreamps[ p ];
			bad += std::string( a.name ) != b.name || a.kind != model::Kind::Triode || a.mu != b.mu || a.ex != b.ex || a.kg1 != b.kg1 || a.kp != b.kp
			       || a.kvb != b.kvb || a.rgi != b.rgi;
		}
		for( int p = 0; p < model::kPowerCount; ++p )
		{
			const model::Valve& a = model::PowerValve( p );
			const Stated& b       = kStatedPowers[ p ];
			bad += std::string( a.name ) != b.name || ( a.kind == model::Kind::Pentode ) != b.pentode || a.mu != b.mu || a.ex != b.ex || a.kg1 != b.kg1
			       || a.kp != b.kp || a.kvb != b.kvb || a.rgi != b.rgi || ( b.pentode && a.kg2 != b.kg2 );
			const model::PowerCircuit& c = model::PowerCircuitOf( p );
			bad += c.bplus != kStatedCircuits[ p ].bplus || c.ratedWatts != kStatedCircuits[ p ].watts || c.raa != kStatedCircuits[ p ].raa;
		}
		bad += model::kPreampSupply != kSupply || model::kPlateLoad != kRa || model::kGridLeak != kLeak || model::kInputStopper != kStopper
		       || model::kDriverSource != kDriver || model::kDefaultIdle != kIdle70;
		failures += report( bad == 0, "model: the eight valves are Koren's Tube.lib, parameter for parameter, and the circuit is the README's (%d differ)",
		                    bad );
	}

	//--- Koren's law, and the plugin's analytic partials.
	{
		double worst = 0.0, worstPartial = 0.0;
		auto sweep = [ & ]( const model::Valve& a, const Stated& b, double screen, double egLo, double egHi ) {
			for( int i = 0; i <= 40; ++i )
				for( int j = 1; j <= 40; ++j )
				{
					const double eg = egLo + ( egHi - egLo ) * i / 40.0, ep = 500.0 * j / 40.0;
					const double mine = model::PlateCurrent( a, eg, ep, screen ).ip, theirs = koren( b, eg, ep, screen );
					//Koren's LOG( 1 + EXP( z ) ), written as he writes it, loses
					//the 1's rounding (1.1e-16) against a small softplus L, and
					//the current goes as L^X: X 1.1e-16 / L relative, a few
					//times over for the rest of the arithmetic.
					const double z = !b.pentode ? b.kp * ( 1.0 / b.mu + eg / std::sqrt( b.kvb + ep * ep ) ) : b.kp * ( 1.0 / b.mu + eg / screen );
					const double L = z > 30.0 ? z : std::log1p( std::exp( z ) );
					const double tol = 8.0 * b.ex * 1.1e-16 * ( 1.0 + 1.0 / L ) + 1e-14;
					if( theirs > 0.0 )
						worst = std::max( worst, std::fabs( mine - theirs ) / theirs / tol );
					const model::Current c = model::PlateCurrent( a, eg, ep, screen );
					const double h   = 1e-4;
					const double dEp = ( koren( b, eg, ep + h, screen ) - koren( b, eg, ep - h, screen ) ) / ( 2 * h );
					const double dEg = ( koren( b, eg + h, ep, screen ) - koren( b, eg - h, ep, screen ) ) / ( 2 * h );
					if( theirs > 1e-6 )
						worstPartial = std::max( { worstPartial, std::fabs( c.dEp - dEp ) / std::max( std::fabs( dEp ), 1e-9 ),
						                           std::fabs( c.dEg - dEg ) / std::max( std::fabs( dEg ), 1e-9 ) } );
				}
		};
		for( int p = 0; p < model::kPreampCount; ++p )
			sweep( model::PreampValve( p, perturb ), kStatedPreamps[ p ], 0.0, -20.0, 2.0 );
		for( int p = 0; p < model::kPowerCount; ++p )
			sweep( model::PowerValve( p ), kStatedPowers[ p ], kStatedCircuits[ p ].bplus, -120.0, 5.0 );
		failures += report( worst <= 1.0, "model: Koren's plate current over 41 x 40 points per valve, worst %.2f of the rounding bound", worst );
		failures += report( worstPartial <= 1e-5, "model: the analytic partials against central differences, worst %.2g relative", worstPartial );
	}

	//--- Operating points and the load lines.
	{
		double worstQ = 0.0, worstLine = 0.0, worstSmall = 0.0, worstPlate = 0.0;
		for( int p = 0; p < model::kPreampCount; ++p )
			for( const double fraction : { 0.05, 0.5, 0.95 } )
			{
				const model::Valve& a = model::PreampValve( p, perturb );
				const Stated& b       = kStatedPreamps[ p ];
				const double eg       = statedCutoff( b, kSupply ) * ( 1.0 - fraction );
				const model::StageQ q = model::PreampQuiescent( a, model::PreampBiasGrid( a, fraction ), 1.0 );
				const StatedQ s       = statedQuiescent( b, eg );
				worstQ     = std::max( { worstQ, std::fabs( q.vp - s.vp ), std::fabs( q.eg - eg ) } );
				worstLine  = std::max( worstLine, std::fabs( q.vp - ( kSupply - q.ip * kRa ) ) );
				worstSmall = std::max( { worstSmall, std::fabs( q.gm - s.gm ) / s.gm, std::fabs( q.rp - s.rp ) / s.rp } );
				for( const double open : { eg - 30.0, eg - 1.0, eg + 0.3, 0.0, 2.0, 40.0 } )
					worstPlate = std::max( worstPlate, std::fabs( model::PreampPlate( a, q, open, kStopper, 1.0, 0 ) - statedPlate( b, s, open, kStopper ) ) );
			}
		failures += report( worstQ <= 1e-6, "model: every preamp's operating point where the harness puts it, worst %.2g V", worstQ );
		failures += report( worstLine <= 1e-9, "model: each on its load line B+ - Ip R_a, worst %.2g V", worstLine );
		failures += report( worstSmall <= 1e-5, "model: gm and r_p at it, worst %.2g relative", worstSmall );
		failures += report( worstPlate <= 1e-6, "model: the plate on the AC load line, with grid current, worst %.2g V", worstPlate );
	}

	//--- The power stage: bias, and both load lines.
	{
		double worstBias = 0.0, worstWatts = 0.0, worstOut = 0.0, worstLoad = 0.0;
		for( int pv = 0; pv < model::kPowerCount; ++pv )
			for( const double idle : { 0.05, 0.7, 1.0 } )
			{
				const StatedPower& c = kStatedCircuits[ pv ];
				const model::PowerQ q = model::PowerQuiescent( pv, model::PowerStage::PushPull, idle, 1.0, 1.0, 0 );
				const double bias     = statedPowerBias( pv, idle );
				worstBias = std::max( worstBias, std::fabs( q.bias - bias ) );
				if( q.bias < 0.0 )
					worstWatts = std::max( worstWatts, std::fabs( q.iq * c.bplus - idle * c.watts ) );
				worstLoad = std::max( worstLoad, std::fabs( q.seLoad - statedSeLoad( pv ) ) );
				const model::PowerQ se = model::PowerQuiescent( pv, model::PowerStage::SingleEnded, idle, 1.0, 1.0, 0 );
				for( const double d : { -100.0, -30.0, -5.0, 3.0, 20.0, 80.0 } )
					worstOut = std::max( { worstOut, std::fabs( model::PowerOut( pv, model::PowerStage::PushPull, q, d, 1.0, 1.0, 0 ) - statedPushPull( pv, bias, d ) ),
					                       std::fabs( model::PowerOut( pv, model::PowerStage::SingleEnded, se, d, 1.0, 1.0, 0 ) - statedSingleEnded( pv, bias, d ) ) } );
			}
		failures += report( worstBias <= 1e-6, "model: each power valve biased where its idle says, worst %.2g V", worstBias );
		failures += report( worstWatts <= 1e-9, "model: idling at the stated fraction of its rating, worst %.2g W", worstWatts );
		failures += report( worstLoad <= 1e-9, "model: the single-ended load is B+^2 / ( 0.7 P_max ), worst %.2g ohm", worstLoad );
		failures += report( worstOut <= 1e-6, "model: single-ended and push-pull outputs on their load lines, worst %.2g V", worstOut );
	}

	//--- The tables' geometry.
	{
		vchain::Settings s;
		s.chain[ 0 ].stages = 2;
		s.chain[ 0 ].power  = model::PowerStage::PushPull;
		s.chain[ 1 ].stages = 2;
		s.chain[ 1 ].power  = model::PowerStage::SingleEnded;
		s.perturb           = perturb;
		vchain::Tables t;
		t.Update( s );
		int rows = 0, centre = 0, knee = 0, reach = 0;
		for( int chain = 0; chain < 2; ++chain )
			for( int kind = 0; kind < vchain::kKinds; ++kind )
			{
				const vchain::Table& table = t.Row( vchain::RowIndex( chain, 1, kind ) );
				if( !table.built )
					continue;
				++rows;
				centre += table.values[ vchain::kCentre ] == 0.0f;
				const double k = kind == vchain::kPower ? -t.PowerQuiescent( chain, 1 ).bias : -t.Vgq( chain );
				const double at = kind == vchain::kPower ? k : 0.0;
				bool onNode = false;
				for( int i = 0; i < vchain::kNodes; ++i )
					onNode = onNode || std::fabs( table.warp.At( i ) - at ) <= 1e-9 * std::max( 1.0, std::fabs( at ) );
				knee += onNode;
				reach += table.warp.At( 0 ) <= table.warp.c - 1000.0 && table.warp.At( vchain::kNodes - 1 ) >= table.warp.c + 1000.0;
			}
		failures += report( rows == 6 && centre == rows && knee == rows && reach == rows,
		                    "model: %d tables -- rest is the centre node and reads exactly 0 (%d), the grid's knee is on a node (%d), a kilovolt each side (%d)",
		                    rows, centre, knee, reach );
		failures += report( t.Evaluate( 0, 1, 0.0 ) == 0.0 && t.Evaluate( 1, 1, 0.0 ) == 0.0, "model: a chain at rest reads exactly 0 V" );
	}

	//--- The control laws.
	{
		namespace c = valvefx::controls;
		auto near = [ & ]( double a, double b ) { return std::fabs( a - b ) <= 1e-12 * std::max( 1.0, std::fabs( b ) ); };
		const bool ok = near( c::DriveVolts( 0.0f ), 0.01 ) && near( c::DriveVolts( 1.0f ), std::pow( 10.0, 2.5 ) ) && near( c::MasterGain( 0.0f ), 0.01 )
		                && near( c::MasterGain( 1.0f ), 100.0 ) && near( c::BiasFraction( 0.0f ), 0.05 ) && near( c::BiasFraction( 1.0f ), 0.95 )
		                && near( c::IdleFraction( 0.0f ), 0.05 ) && near( c::IdleFraction( 1.0f ), 1.0 ) && near( c::InterstageGain( 0.5f ), 0.1 )
		                && near( c::InterstageGain( 1.0f ), 1.0 ) && near( c::MismatchFraction( 1.0f ), 0.25 ) && c::Stages( 2.4f ) == 2
		                && c::Stages( 9.0f ) == 4 && c::OptionIndex( 1.6f, 3 ) == 2 && c::OptionIndex( 7.0f, 3 ) == 2
		                && near( c::DriveVolts( 0.5f ), std::pow( 10.0, 0.25 ) ) && near( c::MasterGain( 0.25f ), 0.1 )
		                && near( c::IdleFraction( 0.5f ), 0.525 ) && near( c::InterstageGain( 0.0f ), 0.01 ) && near( c::RestLevel( 0.25f ), 0.25 );
		failures += report( ok, "model: the control laws are the README's" );
	}

	//--- The chroma's constants.
	{
		bool cosOk = true;
		for( int k = 0; k < vchain::kTaps; ++k )
			cosOk = cosOk && vchain::CosTable()[ static_cast< size_t >( k ) ] == static_cast< float >( std::cos( 2.0 * kPi * k / 64.0 ) );
		const double reach = std::hypot( kKu * 0.299, kKv * 0.701 );
		failures += report( cosOk && std::fabs( vchain::ChromaReach() - reach ) <= 1e-15,
		                    "model: the subcarrier's 64 cosines, and the largest legal chroma %.6f (red and cyan)", reach );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --names (no GL)
//---------------------------------------------------------------------------
int runNames()
{
	//A host gets 16 characters of a parameter's name, and Arena addresses
	//parameters by that name lower-cased with its spaces removed: two names
	//that reduce to one address are one parameter there.
	Valve plugin;
	std::set< std::string > seen, addresses;
	int bad = 0;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		std::string address;
		for( char c : p.name )
			if( c != ' ' )
				address += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		if( p.name.size() > 16 || !seen.insert( p.name ).second || !addresses.insert( address ).second )
			++bad;
	}

	//The name the host reads: plugMain's info block, 16 bytes, not
	//null-terminated.
	const FFMixed info            = plugMain( FF_GET_INFO, FFMixed{ 0 }, 0 );
	const PluginInfoStruct* block = static_cast< const PluginInfoStruct* >( info.PointerValue );
	std::string name, id;
	if( block )
	{
		name.assign( block->PluginName, strnlen( block->PluginName, 16 ) );
		id.assign( block->PluginUniqueID, 4 );
	}
	return report( bad == 0 && block && name == "SW Valve" && id == "VA01" && block->PluginType == FF_EFFECT,
	               "names: %zu parameters, unique as host addresses and within 16 characters; the host reads '%s' / %s / %s", seen.size(), name.c_str(), id.c_str(),
	               block && block->PluginType == FF_EFFECT ? "effect" : "not an effect" );
}

//---------------------------------------------------------------------------
// --negative
//---------------------------------------------------------------------------
struct NegativeControl
{
	const char* what;
	int failuresSeen;
};

int summariseNegatives( const std::vector< NegativeControl >& controls )
{
	int failures = 0;
	for( const NegativeControl& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		++g_checks;
		std::printf( "negative %-58s %s  %s\n", c.what, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
		{
			++failures;
			++g_failures;
		}
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed model is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

/// Run a check against a perturbation quietly, and report whether it failed
/// -- without counting its failures as the run's.
template< typename F >
int caught( F&& check )
{
	const int checks = g_checks, failures = g_failures;
	const bool wasQuiet = g_quiet;
	g_quiet             = true;
	const int seen      = check();
	g_quiet             = wasQuiet;
	g_checks            = checks;
	g_failures          = failures;
	return seen;
}

int runNegativeOffline()
{
	return summariseNegatives( {
		{ "model: the 12AX7's mu typed as 10", caught( [] { return runModel( model::kPerturbWrongMu ); } ) },
	} );
}

int runNegative( int W, int H )
{
	return summariseNegatives( {
		{ "gain: the 12AX7's mu typed as 10", caught( [ & ] { return runGain( W, H, model::kPerturbWrongMu ); } ) },
		{ "knee: the grid never conducts", caught( [ & ] { return runKnee( W, H, model::kPerturbNoGridCurrent ); } ) },
		{ "harmonics: the pair's second grid left at bias", caught( [ & ] { return runHarmonics( W, H, model::kPerturbUndrivenHalf ); } ) },
		{ "crossover: each half on its own load line", caught( [ & ] { return runCrossover( W, H, model::kPerturbSplitLoadLine ); } ) },
		{ "hue: U and V through the chroma chain as signals", caught( [ & ] { return runHue( W, H, model::kPerturbBasebandChroma ); } ) },
		{ "dg: Composite's subcarrier round the valve", caught( [ & ] { return runDg( W, H, model::kPerturbBypassCarrier ); } ) },
		{ "reference: the grid never conducts", caught( [ & ] { return runReference( W, H, model::kPerturbNoGridCurrent ); } ) },
	} );
}

//---------------------------------------------------------------------------
int runChecks( const std::vector< std::string >& checks, int W, int H, int perturb, bool allowNoGL )
{
	//The checks with no GL first; a context only if a rendering one asks.
	bool needGL     = false;
	bool offlineRan = false;
	for( const std::string& check : checks )
	{
		if( check == "--model" )
			runModel( perturb );
		else if( check == "--names" )
			runNames();
		else if( check == "--negative-offline" )
		{
			runNegativeOffline();
			offlineRan = true;
		}
		else
		{
			needGL = true;
			continue;
		}
		std::printf( "\n" );
	}
	if( offlineRan )
		std::printf( "   OFFLINE: --gain, --knee, --harmonics, --crossover, --hue, --dg, --polarity,\n"
		             "   --identity, --reference, --curve and their negative controls were NOT run.\n"
		             "   Nothing here drew a pixel through a GL driver; the shaders were not\n"
		             "   exercised, only (in CI) compiled by glslc.\n\n" );

	if( needGL )
	{
		CGLContextObj context = createContext();
		if( context == nullptr && allowNoGL )
			std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
			             "         The rendering checks and their negative controls were NOT run.\n" );
		else if( context == nullptr )
		{
			std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
			++g_failures;
		}
		else
		{
			for( const std::string& check : checks )
			{
				if( check == "--gain" )
					runGain( W, H, perturb );
				else if( check == "--knee" )
					runKnee( W, H, perturb );
				else if( check == "--harmonics" )
					runHarmonics( W, H, perturb );
				else if( check == "--crossover" )
					runCrossover( W, H, perturb );
				else if( check == "--hue" )
					runHue( W, H, perturb );
				else if( check == "--dg" )
					runDg( W, H, perturb );
				else if( check == "--polarity" )
					runPolarity( W, H, perturb );
				else if( check == "--identity" )
					runIdentity( W, H, perturb );
				else if( check == "--reference" )
					runReference( W, H, perturb );
				else if( check == "--curve" )
					runCurve( W, H, perturb );
				else if( check == "--negative" )
					runNegative( W, H );
				else
					continue;
				std::printf( "\n" );
			}
			CGLSetCurrentContext( nullptr );
			CGLDestroyContext( context );
		}
	}
	std::printf( "%d checks, %d failed\n", g_checks, g_failures );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
/// Per frame: the GPU's own time for the plugin's passes (a GL_TIME_ELAPSED
/// query round ProcessOpenGL -- a wall clock over a batch of identical frames
/// measured 36 us at 4K here, which no GPU can shade, so the driver was
/// overlapping or eliding them), and the CPU's time inside ProcessOpenGL. The
/// median of `frames`, after a warm-up.
struct BenchResult
{
	double gpu = -1.0, cpu = -1.0;
};

BenchResult benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	BenchResult result;
	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		applySetting( session.plugin, setting, error );
	}
	if( !session.begin( width, height ) )
		return result;

	//Uploaded once: the upload is not in the figure.
	session.render( buildCard( width, height ) );
	for( int frame = 0; frame < 10; ++frame )
		session.renderUploaded();
	glFinish();

	std::vector< GLuint > queries( static_cast< size_t >( frames ) );
	glGenQueries( frames, queries.data() );
	std::vector< double > cpu;
	unsigned char pixel[ 4 ];
	for( int i = 0; i < frames; ++i )
	{
		glBeginQuery( GL_TIME_ELAPSED, queries[ static_cast< size_t >( i ) ] );
		const auto start = std::chrono::steady_clock::now();
		session.renderUploaded();
		cpu.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
		glEndQuery( GL_TIME_ELAPSED );
		//One pixel read back: each frame is finished and used before the next.
		glReadPixels( 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel );
	}
	std::vector< double > gpu;
	for( GLuint q : queries )
	{
		GLuint64 ns = 0;
		glGetQueryObjectui64v( q, GL_QUERY_RESULT, &ns );
		gpu.push_back( static_cast< double >( ns ) / 1e6 );
	}
	glDeleteQueries( frames, queries.data() );
	std::sort( gpu.begin(), gpu.end() );
	std::sort( cpu.begin(), cpu.end() );
	result.gpu = gpu[ gpu.size() / 2 ];
	result.cpu = cpu[ cpu.size() / 2 ];
	session.end();
	return result;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720 ", 1280, 720 }, { "1920x1080", 1920, 1080 }, { "3840x2160", 3840, 2160 } };
	std::printf( "median of %d frames after a warm-up: GPU time of the plugin's passes (timer query), CPU time in ProcessOpenGL\n\n", frames );
	std::printf( "resolution   defaults (GPU / CPU)   4 stages + PP x3     Y/C, 4 chroma stages   Composite\n" );
	std::vector< std::string > heavy = settings;
	for( const char* s : { "Stages=4", "Mismatch=0.4", "Power Stage=2" } )
		heavy.push_back( s );
	std::vector< std::string > yc = settings;
	yc.push_back( "Signal=1" );
	yc.push_back( "Chroma Stages=4" );
	std::vector< std::string > composite = settings;
	composite.push_back( "Signal=2" );
	for( const Size& size : sizes )
	{
		const BenchResult a = benchAt( settings, size.width, size.height, frames );
		const BenchResult b = benchAt( heavy, size.width, size.height, frames );
		const BenchResult c = benchAt( yc, size.width, size.height, frames );
		const BenchResult d = benchAt( composite, size.width, size.height, frames );
		std::printf( "%s    %6.3f / %5.3f ms    %6.3f / %5.3f ms    %6.3f / %5.3f ms    %6.3f / %5.3f ms\n", size.name, a.gpu, a.cpu, b.gpu, b.cpu,
		             c.gpu, c.cpu, d.gpu, d.cpu );
	}
	std::printf( "\nEach frame: the display pass (every channel through its chain of table\n"
	             "lookups), plus in Y/C and Composite a 256 x 65 describing-function pass and,\n"
	             "on the CPU, the normalisation's few dozen chain evaluations. Tables are\n"
	             "solved on the CPU only when a setting they depend on moves: not in these\n"
	             "figures, which hold the settings still.\n" );
	return 0;
}

/// What a settings change costs on the CPU: every table solved again.
int runSolveCost()
{
	using clock = std::chrono::steady_clock;
	vchain::Settings s;
	s.chain[ 0 ].stages = 4;
	double best         = 1e9;
	for( int i = 0; i < 3; ++i )
	{
		vchain::Tables t;
		const auto start = clock::now();
		t.Update( s );
		best = std::min( best, std::chrono::duration< double, std::milli >( clock::now() - start ).count() );
	}
	std::printf( "solving one chain's three tables (first, later, push-pull): %.2f ms\n", best );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace sh = valvefx::shaders;
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", sh::kVertex },
		{ "tables.frag", sh::TablesSource() },
		{ "display.frag", sh::DisplaySource() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

/// A standard control is linear between keys; an option, boolean, event or
/// integer steps (holds each key until the next key's frame).
float valueAt( const Track& track, int frame, bool steps )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame < track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( steps )
				return a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"vatest -- render and measure the Valve effect\n"
		"\n"
		"  --out PATH          render the test card through the plugin (default /tmp/valve.png)\n"
		"  --clip PATH         ... or a raw RGBA frame at --size instead of the card\n"
		"  --size WxH          raster (default 1280x720); --width N / --height N also accepted\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options). Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --gain              the slope at mid-grey is the stage's small-signal gain\n"
		"  --knee              past 0 V the grid conducts: the slope drops by r_g / ( r_g + R_s )\n"
		"  --harmonics         H2/H1 of one valve; none from a matched pair; twice from twice the mismatch\n"
		"  --crossover         a cold pair notches at the centre; the centre slope is the composite load line's\n"
		"  --hue               Y/C keeps hue and follows the describing function; RGB moves hue\n"
		"  --dg                Composite: chroma gain follows f'( Y ), strong colour lifts luma by f'' a^2 / 4\n"
		"  --polarity          every common-cathode stage inverts\n"
		"  --identity          no valves is a wire; a whisper of drive is the input; Mix 0; alpha\n"
		"  --reference         every pixel against the CPU's run of the same tables, and those against the model\n"
		"  --curve             Show Curve draws the curve the picture follows\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed model (bits in Valves.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --model             the plugin's valves, solvers, tables and control laws against the statements\n"
		"  --names             nothing the host will silently truncate; the host reads SW Valve / VA01\n"
		"  --offline           both, and their negative controls; says loudly what it skipped. For CI.\n"
		"  --allow-no-gl       with the rendering checks: SKIP loudly, not FAIL, when no GL 4.1 context exists\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K (and a table solve on the CPU)\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}

} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/valve.png";
	std::string scriptPath;
	std::string dumpDir;
	std::string clipPath;
	int width      = 1280;
	int height     = 720;
	int failRender = -1;
	int perturb    = 0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--gain", "--knee", "--harmonics", "--crossover", "--hue", "--dg",
		                                       "--polarity", "--identity", "--reference", "--curve", "--negative" };
	const std::set< std::string > offline  = { "--model", "--names", "--negative-offline" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--clip" && hasNext )
			clipPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			++i;//accepted for the fleet's sweep, which passes it; every frame here is the same frame
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( argument == "--offline" )
			for( const char* m : { "--model", "--names", "--negative-offline" } )
				checks.push_back( m );
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 )
	{
		std::fprintf( stderr, "width and height must be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Valve plugin;
		std::printf( "%3s  %-20s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-20s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
		return runChecks( checks, width, height, perturb, allowNoGL );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( wantBench )
	{
		runSolveCost();
		return finish( runBench( settings, 60 ) );
	}

	Session session;
	session.floatOutput = false;
	session.plugin.SetPerturbForTest( perturb );
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		std::map< unsigned int, std::pair< Track, bool > > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				const unsigned int type = session.plugin.GetParamType( static_cast< unsigned int >( index ) );
				automation[ static_cast< unsigned int >( index ) ] = { entry.second, type != FF_TYPE_STANDARD };
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider would.
			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second.first, index, track.second.second ) );

			const bool rendered = index != failRender && session.render( frame );
			if( !rendered )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	bool ok = false;
	if( !clipPath.empty() )
	{
		std::ifstream clip( clipPath, std::ios::binary );
		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		if( !clip.read( reinterpret_cast< char* >( frame.data() ), static_cast< std::streamsize >( frame.size() ) ) )
		{
			std::fprintf( stderr, "%s is not one %dx%d RGBA frame\n", clipPath.c_str(), width, height );
			return finish( 1 );
		}
		ok = session.render( frame );
	}
	else
		ok = session.render( buildCard( width, height ) );
	if( !ok )
		return finish( 1 );

	const std::vector< unsigned char > image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d)\n", outPath.c_str(), width, height );
	return finish( 0 );
}
