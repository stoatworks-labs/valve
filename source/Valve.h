#pragma once

#include "Chain.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

#include <string>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
	Valve -- the picture through a valve amplifier, as an FFGL effect.

	**The one idea.** A valve's transfer curve is not drawn: it is the valve's
	plate-current law (Koren's) meeting the circuit's load line. Put the video
	level on the grid of a real stage and solve for the plate; chain stages
	the way a guitar amplifier does -- preamp triodes, a phase inverter, a
	power pair into an output transformer -- and the overdrive falls out:
	the gain of the operating point, a soft fade into cutoff against a hard
	knee where the grid conducts, even harmonics from one valve and their
	cancellation in a pair, crossover when the pair is biased cold.

	**Three ways to feed it.** RGB: each channel its own amplifier. Y/C: luma
	through one chain, chroma as a subcarrier through another, so the hue
	survives. Composite: both through one chain, so the colour's gain depends
	on the brightness it rides on.

	**Two processors.** The CPU solves each stage's curve in double, once per
	settings change, into a table (`Chain.h`); the GPU only looks up, scales
	and adds (`Shaders.h`). Nothing carries across frames: a valve chain is
	memoryless. See AGENTS.md for the traps and what is verified.
*/
class Valve : public CFFGLPlugin
{
public:
	Valve();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. Read by vatest; the plugin's own operation never uses
	//--- them, and the perturbation is always 0 outside the harness.

	/// Negative-control hooks, a bitmask of `model::Perturb`.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}

	/// The tables and the normalisation of the last frame rendered.
	const valvefx::chain::Tables& TablesForTest() const
	{
		return tables;
	}
	const valvefx::chain::Norm& NormForTest() const
	{
		return norm;
	}

	/// The settings the parameters stand for, in physical units: what
	/// ProcessOpenGL would hand the tables.
	valvefx::chain::Settings SettingsForTest() const
	{
		return currentSettings();
	}

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Signal
		PT_SIGNAL,
		PT_REST,
		PT_INTERSTAGE,
		PT_MISMATCH,

		//Luma / RGB
		PT_STAGES,
		PT_PREAMP,
		PT_DRIVE,
		PT_BIAS,
		PT_POWER_STAGE,
		PT_POWER_VALVE,
		PT_MASTER,
		PT_POWER_BIAS,

		//Chroma
		PT_C_STAGES,
		PT_C_PREAMP,
		PT_C_DRIVE,
		PT_C_BIAS,
		PT_C_POWER_STAGE,
		PT_C_POWER_VALVE,
		PT_C_MASTER,
		PT_C_POWER_BIAS,

		//Output
		PT_OUTPUT,
		PT_POLARITY,
		PT_SHOW_CURVE,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	valvefx::chain::Settings currentSettings() const;
	void declareChain( FFUInt32 first, const char* prefix );
	void setLibrary( GLuint program, const valvefx::chain::Settings& s );

	ffglex::FFGLShader tablesShader;
	ffglex::FFGLShader displayShader;
	ffglex::FFGLScreenQuad quad;

	valvefx::PassBuffer describing;///< 256 x 65: the chroma's describing functions
	GLuint lutTexture = 0;         ///< kNodes x kRows, R32F: every stage's curve

	valvefx::chain::Tables tables;
	valvefx::chain::Norm norm;
	bool lutUploaded = false;

	int perturb = 0;

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
