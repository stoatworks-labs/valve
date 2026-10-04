#include "Valve.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ffglex;
using namespace valvefx;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Valve >,// Create method
	"VA01",                // Plugin unique ID of maximum length 4.
	"SW Valve",            // Plugin name
	2,                     // API major version number
	1,                     // API minor version number
	0,                     // Plugin major version number
	1,                     // Plugin minor version number
	FF_EFFECT,             // Plugin type
	"The picture through a valve amplifier, like a guitar through an overdriven amp.\n\nEach stage is a real valve (Koren's models of the 12AX7, 12AT7, 12AU7 and 6DJ8; EL34, 6L6GC, KT88 and 300B) solved on its load line: 0 to 4 preamp triodes, then a single-ended or push-pull power stage. Overdrive, asymmetric clipping, crossover at cold bias and the warmth of even harmonics all come out of the valves, not a drawn curve. Feed it as RGB, as Y/C with the chroma as a subcarrier (hue survives), or as Composite (colour gain follows brightness).\n\nStart with Drive, then Stages and Preamp.",// Plugin description
	"Valve FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr with no current context; a log line must never
/// be the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kSignalNames[] = { "RGB", "Y/C", "Composite" };
const char* const kStageNames[]  = { "Off", "Single-ended", "Push-pull" };
const char* const kOutputNames[] = { "Fit", "Unity", "Plate" };
const char* const kPolarityNames[] = { "Corrected", "As Wired" };

constexpr int kSignalCount   = static_cast< int >( chain::Signal::Count );
constexpr int kStageCount    = static_cast< int >( model::PowerStage::Count );
constexpr int kOutputCount   = static_cast< int >( chain::Output::Count );
constexpr int kPolarityCount = 2;

/// The chain's eight controls, in enum order from its first.
enum ChainOffset : FFUInt32
{
	kStagesAt,
	kPreampAt,
	kDriveAt,
	kBiasAt,
	kPowerStageAt,
	kPowerValveAt,
	kMasterAt,
	kPowerBiasAt,
	kChainControls
};

static_assert( Valve::PT_C_STAGES - Valve::PT_STAGES == kChainControls, "the chains are eight controls apart" );
static_assert( Valve::PT_OUTPUT - Valve::PT_C_STAGES == kChainControls, "the chroma chain is eight controls" );

} // namespace

//---------------------------------------------------------------------------
Valve::Valve()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//---------------------------------------------------------------------
	// Defaults: a two-stage 12AX7 preamp into a push-pull pair of EL34s at
	// 70 %, driven to an audible -- visible -- crunch, resting at 0.3 rather
	// than mid-grey because most clips are darker than a test card. RGB, so
	// each channel is its own amplifier. The chroma chain (used in Y/C) is
	// one gentle 12AU7 stage.
	//
	// Filled BEFORE any declaration: SetParamInfof reads its default out of
	// GetFloatParameter (compander's trap).
	//---------------------------------------------------------------------
	params[ PT_SIGNAL ]     = static_cast< float >( chain::Signal::RGB );
	params[ PT_REST ]       = 0.3f;//below mid-grey: most footage is darker than the card
	params[ PT_INTERSTAGE ] = 0.5f;//-20 dB
	params[ PT_MISMATCH ]   = 0.0f;

	params[ PT_STAGES ]      = 2.0f;
	params[ PT_PREAMP ]      = static_cast< float >( model::k12AX7 );
	params[ PT_DRIVE ]       = controls::DriveSlider( 1.2 );
	params[ PT_BIAS ]        = 0.5f;
	params[ PT_POWER_STAGE ] = static_cast< float >( model::PowerStage::PushPull );
	params[ PT_POWER_VALVE ] = static_cast< float >( model::kEL34 );
	params[ PT_MASTER ]      = controls::MasterSlider( 0.3 );
	params[ PT_POWER_BIAS ]  = controls::IdleSlider( model::kDefaultIdle );

	params[ PT_C_STAGES ]      = 1.0f;
	params[ PT_C_PREAMP ]      = static_cast< float >( model::k12AU7 );
	params[ PT_C_DRIVE ]       = controls::DriveSlider( 10.0 );
	params[ PT_C_BIAS ]        = 0.5f;
	params[ PT_C_POWER_STAGE ] = static_cast< float >( model::PowerStage::Off );
	params[ PT_C_POWER_VALVE ] = static_cast< float >( model::kEL34 );
	params[ PT_C_MASTER ]      = controls::MasterSlider( 1.0 );
	params[ PT_C_POWER_BIAS ]  = controls::IdleSlider( model::kDefaultIdle );

	params[ PT_OUTPUT ]     = static_cast< float >( chain::Output::Fit );
	params[ PT_POLARITY ]   = 0.0f;
	params[ PT_SHOW_CURVE ] = 0.0f;
	params[ PT_MIX ]        = 1.0f;

	SetOptionParamInfo( PT_SIGNAL, "Signal", kSignalCount, params[ PT_SIGNAL ] );
	for( int i = 0; i < kSignalCount; ++i )
		SetParamElementInfo( PT_SIGNAL, static_cast< unsigned int >( i ), kSignalNames[ i ], static_cast< float >( i ) );
	SetParamInfof( PT_REST, "Rest Level", FF_TYPE_STANDARD );
	SetParamInfof( PT_INTERSTAGE, "Interstage", FF_TYPE_STANDARD );
	SetParamInfof( PT_MISMATCH, "Mismatch", FF_TYPE_STANDARD );

	declareChain( PT_STAGES, "" );
	declareChain( PT_C_STAGES, "Chroma " );

	SetOptionParamInfo( PT_OUTPUT, "Output", kOutputCount, params[ PT_OUTPUT ] );
	for( int i = 0; i < kOutputCount; ++i )
		SetParamElementInfo( PT_OUTPUT, static_cast< unsigned int >( i ), kOutputNames[ i ], static_cast< float >( i ) );
	SetOptionParamInfo( PT_POLARITY, "Polarity", kPolarityCount, params[ PT_POLARITY ] );
	for( int i = 0; i < kPolarityCount; ++i )
		SetParamElementInfo( PT_POLARITY, static_cast< unsigned int >( i ), kPolarityNames[ i ], static_cast< float >( i ) );
	SetParamInfo( PT_SHOW_CURVE, "Show Curve", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses consecutive ids under one header, so each group
	//is a contiguous run of the enum.
	for( FFUInt32 i = PT_SIGNAL; i <= PT_MISMATCH; ++i )
		SetParamGroup( i, "Signal" );
	for( FFUInt32 i = PT_STAGES; i <= PT_POWER_BIAS; ++i )
		SetParamGroup( i, "Luma / RGB" );
	for( FFUInt32 i = PT_C_STAGES; i <= PT_C_POWER_BIAS; ++i )
		SetParamGroup( i, "Chroma" );
	for( FFUInt32 i = PT_OUTPUT; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );

	// The About block. Declared inline: SetParamInfo is protected on
	// CFFGLPlugin and nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Valve effect" );
	diag::init();
}

void Valve::declareChain( FFUInt32 first, const char* prefix )
{
	//The SDK copies a parameter's name into its own std::string (ParamInfo),
	//so a temporary is enough. A host is handed only 16 characters of a
	//parameter's name (Arena addresses parameters by them), so a prefixed
	//chain says "Pwr" where the main one says "Power": "Chroma Power Stage"
	//would arrive as "Chroma Power Sta".
	const std::string p = prefix;
	auto name = [ & ]( const char* base ) {
		std::string b = base;
		if( !p.empty() && b.rfind( "Power ", 0 ) == 0 )
			b = "Pwr " + b.substr( 6 );
		return p + b;
	};

	SetParamInfo( first + kStagesAt, name( "Stages" ).c_str(), FF_TYPE_INTEGER, params[ first + kStagesAt ] );
	SetParamRange( first + kStagesAt, 0.0f, static_cast< float >( controls::kMaxStages ) );

	SetOptionParamInfo( first + kPreampAt, name( "Preamp" ).c_str(), model::kPreampCount, params[ first + kPreampAt ] );
	for( int i = 0; i < model::kPreampCount; ++i )
		SetParamElementInfo( first + kPreampAt, static_cast< unsigned int >( i ), model::PreampValve( i ).name, static_cast< float >( i ) );

	SetParamInfof( first + kDriveAt, name( "Drive" ).c_str(), FF_TYPE_STANDARD );
	SetParamInfof( first + kBiasAt, name( "Bias" ).c_str(), FF_TYPE_STANDARD );

	SetOptionParamInfo( first + kPowerStageAt, name( "Power Stage" ).c_str(), kStageCount, params[ first + kPowerStageAt ] );
	for( int i = 0; i < kStageCount; ++i )
		SetParamElementInfo( first + kPowerStageAt, static_cast< unsigned int >( i ), kStageNames[ i ], static_cast< float >( i ) );

	SetOptionParamInfo( first + kPowerValveAt, name( "Power Valve" ).c_str(), model::kPowerCount, params[ first + kPowerValveAt ] );
	for( int i = 0; i < model::kPowerCount; ++i )
		SetParamElementInfo( first + kPowerValveAt, static_cast< unsigned int >( i ), model::PowerValve( i ).name, static_cast< float >( i ) );

	SetParamInfof( first + kMasterAt, name( "Master" ).c_str(), FF_TYPE_STANDARD );
	SetParamInfof( first + kPowerBiasAt, name( "Power Bias" ).c_str(), FF_TYPE_STANDARD );
}

//---------------------------------------------------------------------------
chain::Settings Valve::currentSettings() const
{
	chain::Settings s;
	for( int c = 0; c < chain::kChains; ++c )
	{
		const FFUInt32 first     = c == chain::kMain ? PT_STAGES : PT_C_STAGES;
		chain::ChainSettings& cs = s.chain[ static_cast< size_t >( c ) ];
		cs.stages     = controls::Stages( params[ first + kStagesAt ] );
		cs.preamp     = controls::OptionIndex( params[ first + kPreampAt ], model::kPreampCount );
		cs.drive      = controls::DriveVolts( params[ first + kDriveAt ] );
		cs.bias       = controls::BiasFraction( params[ first + kBiasAt ] );
		cs.power      = static_cast< model::PowerStage >( controls::OptionIndex( params[ first + kPowerStageAt ], kStageCount ) );
		cs.powerValve = controls::OptionIndex( params[ first + kPowerValveAt ], model::kPowerCount );
		cs.master     = controls::MasterGain( params[ first + kMasterAt ] );
		cs.idle       = controls::IdleFraction( params[ first + kPowerBiasAt ] );
	}
	s.interstage = controls::InterstageGain( params[ PT_INTERSTAGE ] );
	s.mismatch   = controls::MismatchFraction( params[ PT_MISMATCH ] );
	s.rest       = controls::RestLevel( params[ PT_REST ] );
	const auto signal = static_cast< chain::Signal >( controls::OptionIndex( params[ PT_SIGNAL ], kSignalCount ) );
	//Three valve sets are only worth solving when they differ and each
	//channel has its own.
	s.perChannel = signal == chain::Signal::RGB && s.mismatch > 0.0;
	s.perturb    = perturb;
	return s;
}

//---------------------------------------------------------------------------
FFResult Valve::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const std::string* fragment;
		const char* name;
	} const stages[] = {
		{ &tablesShader, &shaders::TablesSource(), "tables" },
		{ &displayShader, &shaders::DisplaySource(), "display" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( shaders::kVertex, stage.fragment->c_str() ) )
			continue;
		//FF_FAIL is invisible to an operator: the effect simply does nothing.
		//This line is the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Valve: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	//Every stage's curve, one row each. Allocated once at its full size: the
	//tables do not depend on the picture.
	glGenTextures( 1, &lutTexture );
	glBindTexture( GL_TEXTURE_2D, lutTexture );
	const std::vector< float > zeros( static_cast< size_t >( chain::kNodes ) * chain::kRows, 0.0f );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32F, chain::kNodes, chain::kRows, 0, GL_RED, GL_FLOAT, zeros.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	//A new context has an empty texture: every table goes up again.
	tables      = chain::Tables();
	lutUploaded = false;

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void Valve::setLibrary( GLuint program, const chain::Settings& s )
{
	//FFGLShader::Set has no array overload. The program is bound by the
	//caller's ScopedShaderBinding.
	float warps[ chain::kRows * 3 ];
	for( int r = 0; r < chain::kRows; ++r )
	{
		const chain::WarpF w = tables.RowWarp( r );
		warps[ r * 3 + 0 ]   = w.c;
		warps[ r * 3 + 1 ]   = w.w;
		warps[ r * 3 + 2 ]   = w.invDu;
	}
	GLint rowOf[ chain::kChains * chain::kChannels ];
	for( int c = 0; c < chain::kChains; ++c )
		for( int ch = 0; ch < chain::kChannels; ++ch )
			rowOf[ c * chain::kChannels + ch ] = tables.RowBase( c, ch );

	GLint stagesOf[ chain::kChains ], powerOf[ chain::kChains ];
	float driveOf[ chain::kChains ], masterOf[ chain::kChains ], vgqOf[ chain::kChains ];
	for( int c = 0; c < chain::kChains; ++c )
	{
		const chain::ChainSettings& cs = s.chain[ static_cast< size_t >( c ) ];
		stagesOf[ c ] = cs.stages;
		powerOf[ c ]  = static_cast< GLint >( cs.power );
		driveOf[ c ]  = static_cast< float >( cs.drive );
		masterOf[ c ] = static_cast< float >( cs.master );
		vgqOf[ c ]    = static_cast< float >( tables.Vgq( c ) );
	}

	glUniform3fv( glGetUniformLocation( program, "WarpOf" ), chain::kRows, warps );
	glUniform1iv( glGetUniformLocation( program, "RowOf" ), chain::kChains * chain::kChannels, rowOf );
	glUniform1iv( glGetUniformLocation( program, "StagesOf" ), chain::kChains, stagesOf );
	glUniform1iv( glGetUniformLocation( program, "PowerOf" ), chain::kChains, powerOf );
	glUniform1fv( glGetUniformLocation( program, "DriveOf" ), chain::kChains, driveOf );
	glUniform1fv( glGetUniformLocation( program, "MasterOf" ), chain::kChains, masterOf );
	glUniform1fv( glGetUniformLocation( program, "VgqOf" ), chain::kChains, vgqOf );
	glUniform1f( glGetUniformLocation( program, "Interstage" ), static_cast< float >( s.interstage ) );
	glUniform1f( glGetUniformLocation( program, "Rest" ), static_cast< float >( s.rest ) );
}

//---------------------------------------------------------------------------
FFResult Valve::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	const chain::Settings settings = currentSettings();
	const auto signal  = static_cast< chain::Signal >( controls::OptionIndex( params[ PT_SIGNAL ], kSignalCount ) );
	const auto output  = static_cast< chain::Output >( controls::OptionIndex( params[ PT_OUTPUT ], kOutputCount ) );
	const bool asWired = controls::OptionIndex( params[ PT_POLARITY ], kPolarityCount ) == 1;

	//---------------------------------------------------------------------
	// 1. The stage curves: solved again only where a setting they depend on
	//    moved, and only those rows uploaded.
	//---------------------------------------------------------------------
	uint32_t changed = tables.Update( settings );
	if( !lutUploaded )
		changed = ( 1u << chain::kRows ) - 1u;
	if( changed )
	{
		glBindTexture( GL_TEXTURE_2D, lutTexture );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
		for( int r = 0; r < chain::kRows; ++r )
		{
			const chain::Table& t = tables.Row( r );
			if( ( changed & ( 1u << r ) ) && t.built )
				glTexSubImage2D( GL_TEXTURE_2D, 0, 0, r, chain::kNodes, 1, GL_RED, GL_FLOAT, t.values.data() );
		}
		glBindTexture( GL_TEXTURE_2D, 0 );
		lutUploaded = true;
	}
	norm = chain::Normalise( tables, signal, output, asWired );

	//---------------------------------------------------------------------
	// 2. Buffers. Every allocation happens here, before anything binds a
	//    texture: FFGLFBO::Initialise sizes its colour texture under a scoped
	//    binding, and every ffglex Scoped* binding CLEARS to 0 on exit.
	//---------------------------------------------------------------------
	//Allocated in every mode -- it is 66 KB -- so the display always has a
	//real texture on the unit its Tables sampler names. An unbound sampler
	//is legal GL and a warning in Apple's driver log on every frame.
	const bool needTables = signal != chain::Signal::RGB;
	if( !describing.Ensure( chain::kTableY, chain::kTableA + 1, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the 256 x 65 describing-function table" );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// 3. The chroma's describing functions, when the signal has a chroma.
	//---------------------------------------------------------------------
	if( needTables )
	{
		ScopedFBOBinding fbo( describing.GetGLID(), ScopedFBOBinding::RB_REVERT );
		describing.ResizeViewPort();
		ScopedShaderBinding shader( tablesShader.GetGLID() );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding lut( lutTexture );
		tablesShader.Set( "Lut", 1 );
		setLibrary( tablesShader.GetGLID(), settings );
		glUniform1fv( glGetUniformLocation( tablesShader.GetGLID(), "Cosines" ), chain::kTaps, chain::CosTable().data() );
		tablesShader.Set( "Reach", static_cast< float >( chain::ChromaReach() ) );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 4. Display.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( displayShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding input( picture.Handle );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding lut( lutTexture );
		ScopedSamplerActivation s2( 2 );
		Scoped2DTextureBinding describe( describing.TextureID() );

		const GLuint program = displayShader.GetGLID();
		displayShader.Set( "InputTexture", 0 );
		displayShader.Set( "Lut", 1 );
		displayShader.Set( "Tables", 2 );
		displayShader.Set( "InWidth", static_cast< int >( picture.Width ) );
		displayShader.Set( "InHeight", static_cast< int >( picture.Height ) );
		displayShader.Set( "VpX", hostViewport[ 0 ] );
		displayShader.Set( "VpY", hostViewport[ 1 ] );
		displayShader.Set( "VpW", hostViewport[ 2 ] );
		displayShader.Set( "VpH", hostViewport[ 3 ] );
		setLibrary( program, settings );

		displayShader.Set( "Mode", static_cast< int >( signal ) );
		displayShader.Set( "Offset", static_cast< float >( norm.offset ) );
		glUniform3f( glGetUniformLocation( program, "Ref" ), static_cast< float >( norm.ref[ 0 ] ), static_cast< float >( norm.ref[ 1 ] ),
		             static_cast< float >( norm.ref[ 2 ] ) );
		glUniform3f( glGetUniformLocation( program, "Scale" ), static_cast< float >( norm.scale[ 0 ] ),
		             static_cast< float >( norm.scale[ 1 ] ), static_cast< float >( norm.scale[ 2 ] ) );
		displayShader.Set( "ChromaScale", static_cast< float >( norm.chromaScale ) );
		displayShader.Set( "Reach", static_cast< float >( chain::ChromaReach() ) );
		displayShader.Set( "CarrierGain", static_cast< float >( chain::Slope( tables, chain::kMain, 1 ) * norm.scale[ 1 ] ) );
		displayShader.Set( "Perturb", perturb );
		displayShader.Set( "MixAmount", params[ PT_MIX ] );
		displayShader.Set( "ShowCurve", params[ PT_SHOW_CURVE ] >= 0.5f ? 1 : 0 );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Valve::DeInitGL()
{
	tablesShader.FreeGLResources();
	displayShader.FreeGLResources();
	quad.Release();
	describing.Destroy();
	if( lutTexture != 0 )
		glDeleteTextures( 1, &lutTexture );
	lutTexture  = 0;
	lutUploaded = false;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Valve::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Valve::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Valve::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Valve::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}
