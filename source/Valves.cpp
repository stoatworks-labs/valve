#include "Valves.h"

#include <algorithm>
#include <cmath>

namespace valvefx::model
{
namespace
{
//---------------------------------------------------------------------------
// Koren's Tube.lib (2001), parameter for parameter. vatest --model types the
// same table in again from the library and holds the two together.
//---------------------------------------------------------------------------
const Valve kPreamps[ kPreampCount ] = {
	//  name     kind           mu    X     kG1    kG2  kP   kVB    RGI
	{ "12AX7", Kind::Triode, 100.0, 1.40, 1060.0, 0.0, 600.0, 300.0, 2000.0 },
	{ "12AT7", Kind::Triode, 60.0, 1.35, 460.0, 0.0, 300.0, 300.0, 2000.0 },
	{ "12AU7", Kind::Triode, 21.5, 1.30, 1180.0, 0.0, 84.0, 300.0, 2000.0 },
	{ "6DJ8", Kind::Triode, 28.0, 1.30, 330.0, 0.0, 320.0, 300.0, 2000.0 },
};

/// The 12AX7 with its mu typed as 10: the negative control for --model.
const Valve kWrongAX7 = { "12AX7", Kind::Triode, 10.0, 1.40, 1060.0, 0.0, 600.0, 300.0, 2000.0 };

const Valve kPowers[ kPowerCount ] = {
	{ "EL34", Kind::Pentode, 11.0, 1.35, 650.0, 4200.0, 60.0, 24.0, 1000.0 },
	{ "6L6GC", Kind::Pentode, 8.7, 1.35, 1460.0, 4500.0, 48.0, 12.0, 1000.0 },
	{ "KT88", Kind::Pentode, 8.8, 1.35, 730.0, 4200.0, 32.0, 16.0, 1000.0 },
	{ "300B", Kind::Triode, 3.95, 1.40, 1550.0, 0.0, 65.0, 300.0, 1000.0 },
};

/// B+, rated plate dissipation, plate-to-plate load. The ratings are the
/// datasheet maxima a bias calculator uses (EL34 25 W, 6L6GC 30 W, KT88 35 W
/// as GEC first rated it, 300B 36 W as Western Electric first rated it); the
/// supplies and loads are the circuit's choices.
const PowerCircuit kCircuits[ kPowerCount ] = {
	{ 450.0, 25.0, 3400.0 },
	{ 450.0, 30.0, 4000.0 },
	{ 500.0, 35.0, 4000.0 },
	{ 400.0, 36.0, 5000.0 },
};

double logistic( double z )
{
	if( z >= 0.0 )
		return 1.0 / ( 1.0 + std::exp( -z ) );
	const double e = std::exp( z );
	return e / ( 1.0 + e );
}

/// The root of an INCREASING function on [lo, hi], where f( lo ) <= 0 <=
/// f( hi ): Newton where it stays inside the bracket, bisection where it
/// does not (Numerical Recipes' rtsafe, in shape). Every solve in this file
/// is one of these, and every one of them is monotone by construction, so
/// the root is unique and the bracket cannot be lost.
template< typename F >
double solveIncreasing( F&& f, double lo, double hi, double guess )
{
	double x = std::clamp( guess, lo, hi );
	for( int iteration = 0; iteration < 200; ++iteration )
	{
		double value = 0.0, slope = 0.0;
		f( x, value, slope );
		if( value == 0.0 )
			return x;
		if( value < 0.0 )
			lo = x;
		else
			hi = x;
		double next = slope > 0.0 ? x - value / slope : 0.5 * ( lo + hi );
		if( !( next > lo && next < hi ) )
			next = 0.5 * ( lo + hi );
		if( std::fabs( next - x ) <= 1e-13 * std::max( 1.0, std::fabs( x ) ) || hi - lo <= 1e-13 * std::max( 1.0, std::fabs( x ) ) )
			return next;
		x = next;
	}
	return x;
}

} // namespace

const Valve& PreampValve( int index, int perturb )
{
	const int i = std::clamp( index, 0, kPreampCount - 1 );
	if( i == k12AX7 && ( perturb & kPerturbWrongMu ) )
		return kWrongAX7;
	return kPreamps[ i ];
}

const Valve& PowerValve( int index )
{
	return kPowers[ std::clamp( index, 0, kPowerCount - 1 ) ];
}

const PowerCircuit& PowerCircuitOf( int index )
{
	return kCircuits[ std::clamp( index, 0, kPowerCount - 1 ) ];
}

double PowerSupply( int valve )
{
	return PowerCircuitOf( valve ).bplus;
}

double Softplus( double z )
{
	return z > 0.0 ? z + std::log1p( std::exp( -z ) ) : std::log1p( std::exp( z ) );
}

Current PlateCurrent( const Valve& v, double eg, double ep, double screen, double scale )
{
	Current c = { 0.0, 0.0, 0.0 };
	const double kg1 = v.kg1 * scale;
	if( v.kind == Kind::Triode )
	{
		if( ep <= 0.0 )
			return c;
		const double s  = std::sqrt( v.kvb + ep * ep );
		const double z  = v.kp * ( 1.0 / v.mu + eg / s );
		const double e1 = ep / v.kp * Softplus( z );
		if( e1 <= 0.0 )
			return c;
		const double sig    = logistic( z );
		const double dIdE1  = 2.0 * v.ex * std::pow( e1, v.ex - 1.0 ) / kg1;
		const double dE1dEp = Softplus( z ) / v.kp - ep * ep * eg * sig / ( s * s * s );
		const double dE1dEg = ep * sig / s;
		c.ip                = 2.0 * std::pow( e1, v.ex ) / kg1;
		c.dEp               = dIdE1 * dE1dEp;
		c.dEg               = dIdE1 * dE1dEg;
		return c;
	}

	const double z  = v.kp * ( 1.0 / v.mu + eg / screen );
	const double e1 = screen / v.kp * Softplus( z );
	if( e1 <= 0.0 )
		return c;
	const double plate = std::max( ep, 0.0 );
	const double knee  = std::atan( plate / v.kvb );
	const double law   = 2.0 * std::pow( e1, v.ex ) / kg1;
	c.ip               = law * knee;
	c.dEp              = ep > 0.0 ? law / v.kvb / ( 1.0 + ( plate / v.kvb ) * ( plate / v.kvb ) ) : 0.0;
	c.dEg              = 2.0 * v.ex * std::pow( e1, v.ex - 1.0 ) / kg1 * knee * logistic( z );
	return c;
}

double GridVolts( double open, double rgi, double source, int perturb )
{
	if( open <= 0.0 || ( perturb & kPerturbNoGridCurrent ) )
		return open;
	return open * rgi / ( rgi + source );
}

double CutoffGrid( const Valve& v, double supply )
{
	if( v.kind == Kind::Triode )
		return -std::sqrt( v.kvb + supply * supply ) / v.mu;
	return -supply / v.mu;
}

double PreampBiasGrid( const Valve& v, double fraction )
{
	return CutoffGrid( v, kPreampSupply ) * ( 1.0 - fraction );
}

//---------------------------------------------------------------------------
StageQ PreampQuiescent( const Valve& v, double eg, double scale )
{
	//The DC load line: Vp = B+ - Ip R_a. g( Vp ) = Vp - B+ + R_a Ip rises
	//with Vp because Ip does, g( 0 ) = -B+ and g( B+ ) = R_a Ip >= 0.
	const double vp = solveIncreasing(
		[ & ]( double x, double& value, double& slope ) {
			const Current c = PlateCurrent( v, eg, x, 0.0, scale );
			value           = x - kPreampSupply + kPlateLoad * c.ip;
			slope           = 1.0 + kPlateLoad * c.dEp;
		},
		0.0, kPreampSupply, 0.6 * kPreampSupply );

	const Current c = PlateCurrent( v, eg, vp, 0.0, scale );
	StageQ q;
	q.eg   = eg;
	q.vp   = vp;
	q.ip   = c.ip;
	q.gm   = c.dEg;
	q.rp   = c.dEp > 0.0 ? 1.0 / c.dEp : 1e300;
	q.mu   = q.gm * q.rp;
	q.rout = 1.0 / ( 1.0 / q.rp + 1.0 / AcLoad() );
	return q;
}

double PreampPlate( const Valve& v, const StageQ& q, double open, double source, double scale, int perturb, double guess )
{
	const double eg = GridVolts( open, v.rgi, source, perturb );
	const double rl = AcLoad();
	//The AC load line through the quiescent point: Vp = Vq - ( Ip - Iq ) R_L.
	//At cutoff the plate rises to Vq + Iq R_L, which bounds the bracket.
	const double top = q.vp + rl * q.ip;
	const double vp  = solveIncreasing(
        [ & ]( double x, double& value, double& slope ) {
            const Current c = PlateCurrent( v, eg, x, 0.0, scale );
            value           = x - q.vp + rl * ( c.ip - q.ip );
            slope           = 1.0 + rl * c.dEp;
        },
        0.0, top, guess >= 0.0 ? guess : q.vp );
	return vp - q.vp;
}

//---------------------------------------------------------------------------
namespace
{
/// The power stage's raw output for drive d: single-ended, Vp - B+;
/// push-pull, the half-primary voltage V_h. Not yet measured from rest.
double powerRaw( int valve, PowerStage stage, double bias, double seLoad, double d, double scaleA, double scaleB,
                 int perturb, double* guess = nullptr )
{
	const Valve& v           = PowerValve( valve );
	const PowerCircuit& circ = PowerCircuitOf( valve );
	const double bplus       = circ.bplus;

	if( stage == PowerStage::SingleEnded )
	{
		const double eg = GridVolts( bias + d, v.rgi, kDriverSource, perturb );
		const double iq = PlateCurrent( v, bias, bplus, bplus, scaleA ).ip;
		//The transformer's primary has (ideally) no DC resistance and an AC
		//load R: the plate idles at B+ and swings about it, up to
		//B+ + Iq R at cutoff.
		const double vp = solveIncreasing(
			[ & ]( double x, double& value, double& slope ) {
				const Current c = PlateCurrent( v, eg, x, bplus, scaleA );
				value           = x - bplus + seLoad * ( c.ip - iq );
				slope           = 1.0 + seLoad * c.dEp;
			},
			0.0, bplus + seLoad * iq, guess ? *guess : bplus );
		if( guess )
			*guess = vp;
		return vp - bplus;
	}

	const double g1 = GridVolts( bias + d, v.rgi, kDriverSource, perturb );
	const double g2 = GridVolts( ( perturb & kPerturbUndrivenHalf ) ? bias : bias - d, v.rgi, kDriverSource, perturb );
	const double q4 = circ.raa / 4.0;

	if( perturb & kPerturbSplitLoadLine )
	{
		//The wrong model: each half alone on R_aa / 4 from B+, blind to the
		//current in the other half of the winding.
		auto alone = [ & ]( double g, double scale ) {
			return solveIncreasing(
				[ & ]( double x, double& value, double& slope ) {
					const Current c = PlateCurrent( v, g, x, bplus, scale );
					value           = x - bplus + q4 * c.ip;
					slope           = 1.0 + q4 * c.dEp;
				},
				0.0, bplus, bplus );
		};
		return 0.5 * ( alone( g2, scaleB ) - alone( g1, scaleA ) );
	}

	//The centre-tapped primary: the ampere-turns are the DIFFERENCE of the two
	//currents, and the half-primary voltage V_h = ( R_aa / 4 )( I1 - I2 )
	//pulls one plate down and pushes the other up by the same amount. h( V )
	//rises with V (each current falls as its own plate falls), and the
	//bracket is the one in which both plates stay at or above 0 V.
	const double vh = solveIncreasing(
		[ & ]( double x, double& value, double& slope ) {
			const Current c1 = PlateCurrent( v, g1, bplus - x, bplus, scaleA );
			const Current c2 = PlateCurrent( v, g2, bplus + x, bplus, scaleB );
			value            = x - q4 * ( c1.ip - c2.ip );
			slope            = 1.0 + q4 * ( c1.dEp + c2.dEp );
		},
		-bplus, bplus, guess ? *guess : 0.0 );
	if( guess )
		*guess = vh;
	return vh;
}
} // namespace

PowerQ PowerQuiescent( int valve, PowerStage stage, double idle, double scaleA, double scaleB, int perturb )
{
	const Valve& v           = PowerValve( valve );
	const PowerCircuit& circ = PowerCircuitOf( valve );
	const double bplus       = circ.bplus;

	PowerQ q;
	//The bias that idles the NOMINAL valve at `idle` of its rating with its
	//plate at B+. Current rises with the grid, so this is monotone too. A
	//target beyond what 0 V gives is held at 0 V: the grid cannot be biased
	//positive.
	const double target = idle * circ.ratedWatts / bplus;
	if( PlateCurrent( v, 0.0, bplus, bplus ).ip <= target )
		q.bias = 0.0;
	else
		q.bias = solveIncreasing(
			[ & ]( double x, double& value, double& slope ) {
				const Current c = PlateCurrent( v, x, bplus, bplus );
				value           = c.ip - target;
				slope           = c.dEg;
			},
			-bplus, 0.0, CutoffGrid( v, bplus ) * 0.5 );
	q.iq = PlateCurrent( v, q.bias, bplus, bplus ).ip;

	//The single-ended transformer is chosen for the default idle and does not
	//change when the valve is rebiased: R = B+ / I_q(70 %), the load that
	//lets a class-A plate swing from 0 to 2 B+.
	q.seLoad = bplus * bplus / ( kDefaultIdle * circ.ratedWatts );

	q.vhq = stage == PowerStage::PushPull ? powerRaw( valve, stage, q.bias, q.seLoad, 0.0, scaleA, scaleB, perturb ) : 0.0;
	return q;
}

double PowerOut( int valve, PowerStage stage, const PowerQ& q, double d, double scaleA, double scaleB, int perturb, double* guess )
{
	if( stage == PowerStage::Off )
		return d;
	return powerRaw( valve, stage, q.bias, q.seLoad, d, scaleA, scaleB, perturb, guess ) - ( stage == PowerStage::PushPull ? q.vhq : 0.0 );
}

} // namespace valvefx::model
