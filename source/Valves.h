#pragma once

/**
	The valves and the circuit they sit in. Everything here is double, on the
	CPU, and nothing here touches GL: it is what the stage tables are solved
	from, and what vatest's --model holds against Koren.

	**The valves are Norman Koren's.** "Improved vacuum tube models for SPICE
	simulations", Glass Audio 8(5), 1996, with the parameters of his model
	library Tube.lib (normankoren.com/Audio/Tubemods.zip, 2001), which adds the
	12AT7 and the EL34 to the article's Table 1. One source for all eight.

	  triode   E1 = (Ep / kP) ln( 1 + exp( kP ( 1/mu + Eg / sqrt( kVB + Ep^2 ) ) ) )
	           Ip = 2 E1^X / kG1                                    (E1 > 0)
	  pentode  E1 = (Eg2 / kP) ln( 1 + exp( kP ( 1/mu + Eg / Eg2 ) ) )
	           Ip = 2 E1^X / kG1 atan( Ep / kVB )                   (E1 > 0)

	(Koren writes the current as (|E1|^X + sgn(E1) |E1|^X) / kG1, which is the
	same thing.) Grid current is his RGI with the diode made ideal: past 0 V
	the grid is a resistor to the cathode. The contact potential that starts
	real grid current a fraction of a volt early is not modelled, and the
	README says so.

	**The circuit is stated, not fitted.** A preamp stage is the common
	cathode stage every guitar amplifier starts with: 300 V, 100 k on the
	plate, the cathode bypassed (so it sits at its quiescent), the next grid's
	1 M leak in parallel with the plate load. The power stage is a pair into a
	centre-tapped output transformer, or one valve into one, with the screen
	held at B+. Where a number is a choice rather than physics it says so.
*/
namespace valvefx::model
{

constexpr double kPi = 3.14159265358979323846;

enum class Kind
{
	Triode,
	Pentode
};

struct Valve
{
	const char* name;
	Kind kind;
	double mu;
	double ex;
	double kg1;
	double kg2;///< pentode screen constant; unused here (the screen supply is stiff)
	double kp;
	double kvb;
	double rgi;///< grid-to-cathode resistance once the grid conducts, ohms
};

//---------------------------------------------------------------------------
// The two menus. The order is the host's option order: change it and every
// saved composition's choice moves.
//---------------------------------------------------------------------------
enum Preamp : int
{
	k12AX7,
	k12AT7,
	k12AU7,
	k6DJ8,
	kPreampCount
};

enum Power : int
{
	kEL34,
	k6L6GC,
	kKT88,
	k300B,
	kPowerCount
};

/// `perturb` carries kPerturbWrongMu, which hands back a 12AX7 with a
/// dropped zero: --model's negative control.
const Valve& PreampValve( int index, int perturb = 0 );
const Valve& PowerValve( int index );

/// The power stage's circuit, per valve. B+ and the plate-to-plate load are
/// the typical guitar-amplifier values of their day (a choice, not a claim
/// any check tests); the rated dissipation is the datasheet maximum the bias
/// is set against.
struct PowerCircuit
{
	double bplus;      ///< plate and screen supply, volts
	double ratedWatts; ///< maximum plate dissipation, watts
	double raa;        ///< push-pull plate-to-plate load, ohms
};
const PowerCircuit& PowerCircuitOf( int index );

//---------------------------------------------------------------------------
// The preamp stage and the grids' sources.
//---------------------------------------------------------------------------
constexpr double kPreampSupply  = 300.0;  ///< B+, volts
constexpr double kPlateLoad     = 100e3;  ///< R_a, ohms
constexpr double kGridLeak      = 1e6;    ///< the next grid's leak, ohms
constexpr double kInputStopper  = 68e3;   ///< the first grid's source: the input jack's stopper
constexpr double kDriverSource  = 47e3;   ///< the power grids' source: the phase inverter
constexpr double kDefaultIdle   = 0.70;   ///< the guitar tech's "bias to 70 %"

/// R_a in parallel with the next grid's leak: the load the plate's SIGNAL
/// sees, once the coupling capacitor is a short at signal frequencies.
constexpr double AcLoad()
{
	return kPlateLoad * kGridLeak / ( kPlateLoad + kGridLeak );
}

//---------------------------------------------------------------------------
// Negative-control hooks, a bitmask. Always 0 outside vatest --negative.
//---------------------------------------------------------------------------
enum Perturb : int
{
	kPerturbNoGridCurrent   = 1 << 0,///< the grid never conducts
	kPerturbUndrivenHalf    = 1 << 1,///< the push-pull pair's second grid sits at bias
	kPerturbSplitLoadLine   = 1 << 2,///< each half on its own R_aa/4, blind to the other's current
	kPerturbBasebandChroma  = 1 << 3,///< (shader) U and V through the chroma chain as signals
	kPerturbBypassCarrier   = 1 << 4,///< (shader) Composite: the subcarrier goes round the valve
	kPerturbWrongMu         = 1 << 5,///< the 12AX7's mu typed as 10 (a dropped zero)
};

//---------------------------------------------------------------------------
// Koren's plate current, and its partial derivatives.
//---------------------------------------------------------------------------
struct Current
{
	double ip;   ///< amps
	double dEp;  ///< dIp / dEp, siemens
	double dEg;  ///< dIp / dEg, siemens (the transconductance)
};

/// `scale` multiplies kG1: a valve that draws 1 / scale of the nominal
/// current at any one operating point (Mismatch). The screen voltage is used
/// by pentodes only.
Current PlateCurrent( const Valve& v, double eg, double ep, double screen, double scale = 1.0 );

/// ln( 1 + e^z ) without overflow either way.
double Softplus( double z );

/// The grid seen through its source: below 0 V an open circuit's voltage
/// arrives whole; above it the grid conducts through `rgi` and the source
/// resistance divides it down.
double GridVolts( double open, double rgi, double source, int perturb );

/// The grid voltage at which the plate current is cut off, with the plate at
/// the supply: -sqrt( kVB + B+^2 ) / mu (triodes).
double CutoffGrid( const Valve& v, double supply );

//---------------------------------------------------------------------------
// The preamp stage.
//---------------------------------------------------------------------------
struct StageQ
{
	double eg;   ///< the quiescent grid, volts (fixed: a bypassed cathode)
	double vp;   ///< the quiescent plate, volts
	double ip;   ///< the quiescent current, amps
	double gm;   ///< siemens
	double rp;   ///< ohms
	double mu;   ///< gm rp at the operating point
	double rout; ///< r_p in parallel with the AC load: what the next grid's source is
};

/// The DC operating point on the load line B+ - R_a, for a grid at `eg`.
StageQ PreampQuiescent( const Valve& v, double eg, double scale );

/// The plate's deviation from quiescent, volts, for an open-circuit grid
/// voltage (grid to cathode) `open`, through a source of `source` ohms: on
/// the AC load line through the quiescent point.
/// `guess`, when given, is a plate voltage to start Newton from -- the
/// neighbouring node's answer, when a table is solved outward from rest.
double PreampPlate( const Valve& v, const StageQ& q, double open, double source, double scale, int perturb, double guess = -1.0 );

/// The quiescent grid for a Bias slider at `fraction` of the way from
/// cutoff (0) to 0 V (1).
double PreampBiasGrid( const Valve& v, double fraction );

//---------------------------------------------------------------------------
// The power stage.
//---------------------------------------------------------------------------
enum class PowerStage : int
{
	Off,
	SingleEnded,
	PushPull,
	Count
};

struct PowerQ
{
	double bias;   ///< the grid bias, volts, for the NOMINAL valve
	double iq;     ///< the nominal valve's idle current at B+, amps
	double seLoad; ///< the single-ended load: B+ / I_q at the default idle
	double vhq;    ///< the push-pull half-primary voltage at rest (0 when matched)
};

/// The bias that idles the nominal valve at `idle` of its rated dissipation
/// with the plate at B+, and the rest of the stage's operating point. `scaleA`
/// and `scaleB` are the two halves' kG1 multipliers (the SE stage uses A).
PowerQ PowerQuiescent( int valve, PowerStage stage, double idle, double scaleA, double scaleB, int perturb );

/// The stage's output for a drive of `d` volts on the grid(s), volts,
/// measured from rest: single-ended, the plate's swing about B+ (inverting);
/// push-pull, V_h = ( R_aa / 4 )( I1 - I2 ) (not inverting).
/// `guess` as for PreampPlate: the raw solve's unknown (the plate, single-
/// ended; V_h, push-pull) at a neighbouring drive.
double PowerOut( int valve, PowerStage stage, const PowerQ& q, double d, double scaleA, double scaleB, int perturb,
                 double* guess = nullptr );

/// The valve's own supply: what Plate mode divides by.
double PowerSupply( int valve );

} // namespace valvefx::model
