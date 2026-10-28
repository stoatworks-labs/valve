// The C++ half of the browser demo's cross-check: the REAL plugin class, Valve
// from Valve.cpp, in a headless OpenGL 4.1 context, driven through
// scenario.mjs's settings the way a host drives it (SetFloatParameter, then
// ProcessOpenGL into a framebuffer of its own). After every frame it prints the
// plugin's own state -- the settings currentSettings() made, the tables and
// operating points Tables::Update solved, the normalisation -- and every uniform
// of its two shader programs, read back out of GL. run.mjs prints the same from
// the PORT block of demo/plugin.js; crosscheck.sh compares the two.
//
//     printer < scenario.txt > cpp.txt
//
// Nothing here computes what it prints, with one exception: the `eval`,
// `fund` and `slope` lines call the plugin's own const Tables::Evaluate,
// Fundamental and chain::Slope on the tables the frame left, at fixed
// arguments, so the chain itself is compared too and not only where Normalise
// happens to sample it. Doubles print as their 64 bits, floats as their 32,
// ints in decimal.

#include "Chain.h"
#include "Controls.h"
#include "Valve.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace valvefx;

namespace
{
std::string d( double v )
{
	uint64_t bits;
	std::memcpy( &bits, &v, sizeof bits );
	char buffer[ 24 ];
	std::snprintf( buffer, sizeof buffer, "%016" PRIx64, bits );
	return buffer;
}
std::string f( float v )
{
	uint32_t bits;
	std::memcpy( &bits, &v, sizeof bits );
	char buffer[ 12 ];
	std::snprintf( buffer, sizeof buffer, "%08" PRIx32, bits );
	return buffer;
}
uint32_t fbits( float v )
{
	uint32_t bits;
	std::memcpy( &bits, &v, sizeof bits );
	return bits;
}

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute attributes[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	CGLPixelFormatObj format = nullptr;
	GLint count              = 0;
	if( CGLChoosePixelFormat( attributes, &format, &count ) != kCGLNoError || format == nullptr )
		return nullptr;
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

/// The host's side: an 8-bit clip and an output framebuffer, as Resolume's.
struct Host
{
	std::unique_ptr< Valve > plugin;
	int width = 0, height = 0;
	GLuint output = 0, fbo = 0, clip = 0;
	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct input        = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	GLuint tablesProgram = 0, displayProgram = 0;

	GLuint texture()
	{
		GLuint t = 0;
		glGenTextures( 1, &t );
		glBindTexture( GL_TEXTURE_2D, t );
		std::vector< unsigned char > grey( static_cast< size_t >( width ) * height * 4, 128 );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, grey.data() );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return t;
	}

	bool attach( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin->InitGL( &viewport ) != FF_SUCCESS )
			return false;
		output = texture();
		clip   = texture();
		glGenFramebuffers( 1, &fbo );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0 );
		const bool complete = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		input.Width = input.HardwareWidth = static_cast< FFUInt32 >( w );
		input.Height = input.HardwareHeight = static_cast< FFUInt32 >( h );
		input.Handle                        = clip;
		inputs[ 0 ]                         = &input;
		process.numInputTextures            = 1;
		process.inputTextures               = inputs;
		process.HostFBO                     = fbo;

		//The plugin's two programs, told apart by a uniform only each has.
		tablesProgram = displayProgram = 0;
		for( GLuint id = 1; id < 4096; ++id )
		{
			if( !glIsProgram( id ) )
				continue;
			if( glGetUniformLocation( id, "Cosines" ) >= 0 )
				tablesProgram = id;
			if( glGetUniformLocation( id, "Mode" ) >= 0 )
				displayProgram = id;
		}
		return complete && tablesProgram && displayProgram;
	}

	void detach()
	{
		plugin->DeInitGL();
		if( fbo )
			glDeleteFramebuffers( 1, &fbo );
		if( output )
			glDeleteTextures( 1, &output );
		if( clip )
			glDeleteTextures( 1, &clip );
		fbo = output = clip = 0;
	}

	bool render()
	{
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glViewport( 0, 0, width, height );
		return plugin->ProcessOpenGL( &process ) == FF_SUCCESS;
	}
};

//---------------------------------------------------------------------------
// The uniforms, read back. Each line: u <pass> <name> <values>.
//---------------------------------------------------------------------------
enum class U
{
	Float,
	Int,
	Vec3
};

void uniform( GLuint program, const char* pass, const std::string& name, U kind, int count = 0 )
{
	std::string line = std::string( "u " ) + pass + " " + name;
	auto one = [ & ]( const std::string& n ) {
		const GLint loc = glGetUniformLocation( program, n.c_str() );
		if( loc < 0 )
		{
			line += " inactive";
			return;
		}
		if( kind == U::Int )
		{
			GLint v = 0;
			glGetUniformiv( program, loc, &v );
			line += " " + std::to_string( v );
		}
		else
		{
			GLfloat v[ 3 ] = { 0, 0, 0 };
			glGetUniformfv( program, loc, v );
			line += " " + f( v[ 0 ] );
			if( kind == U::Vec3 )
				line += " " + f( v[ 1 ] ) + " " + f( v[ 2 ] );
		}
	};
	if( count == 0 )
		one( name );
	else
		for( int i = 0; i < count; ++i )
			one( name + "[" + std::to_string( i ) + "]" );
	std::printf( "%s\n", line.c_str() );
}

void library( GLuint program, const char* pass )
{
	uniform( program, pass, "WarpOf", U::Vec3, chain::kRows );
	uniform( program, pass, "RowOf", U::Int, chain::kChains * chain::kChannels );
	uniform( program, pass, "StagesOf", U::Int, chain::kChains );
	uniform( program, pass, "PowerOf", U::Int, chain::kChains );
	uniform( program, pass, "DriveOf", U::Float, chain::kChains );
	uniform( program, pass, "MasterOf", U::Float, chain::kChains );
	uniform( program, pass, "VgqOf", U::Float, chain::kChains );
	uniform( program, pass, "Interstage", U::Float );
	uniform( program, pass, "Rest", U::Float );
}

std::array< uint32_t, chain::kRows > lastHash {};
std::array< bool, chain::kRows > lastBuilt {};

void printState( Host& host )
{
	Valve& p = *host.plugin;
	const chain::Settings s = p.SettingsForTest();
	for( int c = 0; c < chain::kChains; ++c )
	{
		const chain::ChainSettings& cs = s.chain[ static_cast< size_t >( c ) ];
		std::printf( "s %d stages %d preamp %d drive %s bias %s power %d valve %d master %s idle %s\n", c, cs.stages, cs.preamp,
		             d( cs.drive ).c_str(), d( cs.bias ).c_str(), static_cast< int >( cs.power ), cs.powerValve,
		             d( cs.master ).c_str(), d( cs.idle ).c_str() );
	}
	std::printf( "s interstage %s mismatch %s rest %s perChannel %d\n", d( s.interstage ).c_str(), d( s.mismatch ).c_str(),
	             d( s.rest ).c_str(), s.perChannel ? 1 : 0 );

	const chain::Tables& t = p.TablesForTest();
	std::printf( "vgq %s %s\n", d( t.Vgq( 0 ) ).c_str(), d( t.Vgq( 1 ) ).c_str() );
	for( int c = 0; c < chain::kChains; ++c )
	{
		const bool split = c == chain::kMain && s.perChannel;
		for( int ch = split ? 0 : 1; ch <= ( split ? 2 : 1 ); ++ch )
		{
			const model::StageQ& q = t.Quiescent( c, ch );
			std::printf( "q %d %d %s %s %s %s %s %s %s\n", c, ch, d( q.eg ).c_str(), d( q.vp ).c_str(), d( q.ip ).c_str(),
			             d( q.gm ).c_str(), d( q.rp ).c_str(), d( q.mu ).c_str(), d( q.rout ).c_str() );
			if( s.chain[ static_cast< size_t >( c ) ].power != model::PowerStage::Off )
			{
				const model::PowerQ& pq = t.PowerQuiescent( c, ch );
				std::printf( "pq %d %d %s %s %s %s\n", c, ch, d( pq.bias ).c_str(), d( pq.iq ).c_str(), d( pq.seLoad ).c_str(),
				             d( pq.vhq ).c_str() );
			}
		}
	}

	for( int r = 0; r < chain::kRows; ++r )
	{
		const chain::Table& row = t.Row( r );
		uint32_t h = 2166136261u;
		for( float v : row.values )
			h = ( h ^ fbits( v ) ) * 16777619u;
		const chain::WarpF wf = t.RowWarp( r );
		std::printf( "row %d built %d warp %s %s %s wf %s %s %s n %zu hash %08" PRIx32 "\n", r, row.built ? 1 : 0, d( row.warp.c ).c_str(),
		             d( row.warp.w ).c_str(), d( row.warp.du ).c_str(), f( wf.c ).c_str(), f( wf.w ).c_str(), f( wf.invDu ).c_str(),
		             row.values.size(), h );
		if( row.built && ( !lastBuilt[ static_cast< size_t >( r ) ] || lastHash[ static_cast< size_t >( r ) ] != h ) )
		{
			for( size_t i = 0; i < row.values.size(); i += 16 )
			{
				std::string line = "data " + std::to_string( r ) + " " + std::to_string( i );
				for( size_t k = i; k < std::min( i + 16, row.values.size() ); ++k )
					line += " " + f( row.values[ k ] );
				std::printf( "%s\n", line.c_str() );
			}
		}
		lastBuilt[ static_cast< size_t >( r ) ] = row.built;
		lastHash[ static_cast< size_t >( r ) ]  = h;
	}

	const chain::Norm& n = p.NormForTest();
	std::printf( "norm %s %s %s %s %s %s %s %s\n", d( n.offset ).c_str(), d( n.ref[ 0 ] ).c_str(), d( n.ref[ 1 ] ).c_str(),
	             d( n.ref[ 2 ] ).c_str(), d( n.scale[ 0 ] ).c_str(), d( n.scale[ 1 ] ).c_str(), d( n.scale[ 2 ] ).c_str(),
	             d( n.chromaScale ).c_str() );

	//The chain itself, at fixed arguments.
	const double rest = s.rest;
	for( int ch = 0; ch < chain::kChannels; ++ch )
		for( const double x : { -rest, -0.1, 0.0, 0.05, 1.0 - rest } )
			std::printf( "eval 0 %d %s %s\n", ch, d( x ).c_str(), d( t.Evaluate( 0, ch, x ) ).c_str() );
	for( const double x : { -0.2, 0.0, 0.2 } )
		std::printf( "eval 1 1 %s %s\n", d( x ).c_str(), d( t.Evaluate( 1, 1, x ) ).c_str() );
	for( const double a : { 0.1, chain::ChromaReach() } )
	{
		double dc        = 0.0;
		const double one = t.Fundamental( 1, 1, 0.0, a, &dc );
		std::printf( "fund %s %s %s\n", d( a ).c_str(), d( one ).c_str(), d( dc ).c_str() );
	}
	std::printf( "slope %s %s\n", d( chain::Slope( t, 0, 1 ) ).c_str(), d( chain::Slope( t, 1, 1 ) ).c_str() );

	//The uniforms the frame handed GL. The tables pass runs only when the signal
	//has a chroma; otherwise its program still holds an older frame's.
	const int signal = controls::OptionIndex( p.GetFloatParameter( Valve::PT_SIGNAL ), 3 );
	if( signal != 0 )
	{
		library( host.tablesProgram, "tables" );
		uniform( host.tablesProgram, "tables", "Lut", U::Int );
		uniform( host.tablesProgram, "tables", "Cosines", U::Float, chain::kTaps );
		uniform( host.tablesProgram, "tables", "Reach", U::Float );
	}
	const GLuint dp = host.displayProgram;
	library( dp, "display" );
	for( const char* name : { "InputTexture", "Lut", "Tables", "InWidth", "InHeight", "VpX", "VpY", "VpW", "VpH", "Mode" } )
		uniform( dp, "display", name, U::Int );
	uniform( dp, "display", "Offset", U::Float );
	uniform( dp, "display", "Ref", U::Vec3 );
	uniform( dp, "display", "Scale", U::Vec3 );
	for( const char* name : { "ChromaScale", "Reach", "CarrierGain" } )
		uniform( dp, "display", name, U::Float );
	uniform( dp, "display", "Perturb", U::Int );
	uniform( dp, "display", "MixAmount", U::Float );
	uniform( dp, "display", "ShowCurve", U::Int );
}

} // namespace

int main()
{
	CGLContextObj context = createContext();
	if( !context )
	{
		std::fprintf( stderr, "printer: no OpenGL 4.1 context\n" );
		return 2;
	}

	Host host;
	host.plugin = std::make_unique< Valve >();

	//What the constructor declared: name, group, type, elements, range, default.
	Valve& p = *host.plugin;
	for( unsigned int i = 0; i < Valve::PT_ABOUT_FIRST; ++i )
	{
		std::string line = "decl " + std::to_string( i ) + " " + p.GetParamName( i ) + "|" + p.GetParamGroup( i ) + "|"
		                   + std::to_string( p.GetParamType( i ) ) + "|";
		const unsigned int n = p.GetParamType( i ) == FF_TYPE_OPTION ? p.GetNumParamElements( i ) : 0;
		for( unsigned int e = 0; e < n; ++e )
			line += ( e ? "," : "" ) + std::string( p.GetParamElementName( i, e ) );
		if( p.GetParamType( i ) == FF_TYPE_INTEGER )
		{
			const RangeStruct range = p.GetParamRange( i );
			line += "range " + f( range.min ) + ".." + f( range.max );
		}
		line += "|" + f( p.GetFloatParameter( i ) );
		std::printf( "%s\n", line.c_str() );
	}

	std::string text;
	int step = 0;
	bool attached = false;
	while( std::getline( std::cin, text ) )
	{
		std::istringstream in( text );
		std::string word;
		in >> word;
		if( word == "size" )
		{
			int w = 0, h = 0;
			in >> w >> h;
			if( attached )
				host.detach();
			if( !host.attach( w, h ) )
			{
				std::fprintf( stderr, "printer: InitGL failed at %dx%d\n", w, h );
				return 1;
			}
			attached = true;
			std::printf( "size %d %d\n", w, h );
		}
		else if( word == "set" )
		{
			for( unsigned int i = 0; i < Valve::PT_ABOUT_FIRST; ++i )
			{
				std::string hex;
				in >> hex;
				const uint32_t bits = static_cast< uint32_t >( std::stoul( hex, nullptr, 16 ) );
				float v;
				std::memcpy( &v, &bits, sizeof v );
				p.SetFloatParameter( i, v );
			}
			if( !host.render() )
			{
				std::fprintf( stderr, "printer: ProcessOpenGL failed at step %d\n", step );
				return 1;
			}
			std::printf( "step %d\n", step++ );
			printState( host );
		}
	}
	host.detach();
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}
