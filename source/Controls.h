#pragma once

/**
	What a host parameter means.

	Every ranged host parameter is 0..1 (SetParamInfo clamps a STANDARD
	default into 0..1 before a range can be attached), an option parameter's
	range reads back 0..1 whatever its element count -- so an option is its
	element INDEX, rounded and clamped here -- and Stages is a real integer.
	Every conversion to a physical unit lives here and nowhere else; vatest
	states the same laws from their definitions and --model holds the two
	together.
*/
namespace valvefx::controls
{

/// An option's stored value, as an index into its `count` elements.
int OptionIndex( float value, int count );

/// Stages: a real integer, 0 to 4.
int Stages( float value );
constexpr int kMaxStages = 4;

/// Drive: the volts on the first grid for the whole swing from black to
/// white, 10^( -2 + 4.5 v ): 10 mV to 316 V. Logarithmic, because a 12AX7
/// clips at a volt and a 300B needs a hundred.
double DriveVolts( float value );

/// Master: volts on the power grids per volt of the preamp's swing (the
/// phase inverter's gain and the master pot together), 10^( -2 + 4 v ):
/// 0.01 to 100.
double MasterGain( float value );

/// Bias: where the preamp's quiescent grid sits, as a fraction of the way
/// from cutoff to 0 V, 0.05 + 0.9 v: cold at the bottom, hot at the top.
double BiasFraction( float value );

/// Power Bias: the idle dissipation as a fraction of the valve's rating,
/// 0.05 + 0.95 v: 5 % (class B, crossover) to 100 % (class A, hot).
double IdleFraction( float value );

/// Interstage: the loss between one plate and the next grid -- the tone
/// stack's -- -40 + 40 v dB, as a voltage ratio.
double InterstageGain( float value );

/// Rest Level: the picture level that sits at the valves' rest -- where a
/// black-level clamp ahead of the first grid holds the signal -- 0 to 1.
/// Mid-grey by default; lower it for dark footage, so the shadows get the
/// valve's working range instead of its cutoff.
double RestLevel( float value );

/// Mismatch: the spread of kG1 between valves, 0 to 25 %.
double MismatchFraction( float value );

/// The inverses, for declaring defaults in physical units.
float DriveSlider( double volts );
float MasterSlider( double gain );
float IdleSlider( double fraction );

} // namespace valvefx::controls
