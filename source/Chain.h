#pragma once

#include "Valves.h"

#include <array>
#include <vector>

/**
	The stage tables, and the chain the shader runs over them.

	**A stage is a curve.** Every valve stage here is memoryless: the plate's
	deviation from rest is a function of the voltage on its grid and nothing
	else. So each stage is solved once, on the CPU in double, at kNodes grid
	voltages, and the GPU only looks it up. A table depends on the valve, the
	bias, the mismatch and the grid's source -- never on Drive, Master,
	Interstage or the number of stages, which are uniforms, so dragging any of
	those costs nothing.

	**The grid is warped.** A 12AX7's whole working range is three volts and
	a driven grid can be a kilovolt away, so the nodes are evenly spaced in
	u = asinh( ( v - c ) / w ): fine near the quiescent, coarse far out where
	the plate is pinned anyway.

	  - kNodes is odd and the CENTRE node is the quiescent point, so a grid at
	    rest maps to index kCentre exactly and reads exactly 0.
	  - The grid-current knee (0 V on the grid; for a power pair, each grid's
	    own 0 V) is placed ON a node by adjusting w, so linear interpolation
	    never straddles the one kink the curve has.

	**What the GPU does, the CPU can do too.** `Evaluate` runs the shader's
	chain over the same float table with the same float-rounded warp: the
	normalisation constants come from it, and vatest holds the picture to it.
*/
namespace valvefx::chain
{

constexpr int kNodes    = 4097;
constexpr int kCentre   = 2048;
constexpr int kChains   = 2;///< 0: Luma / RGB, 1: Chroma
constexpr int kChannels = 3;
constexpr int kMain     = 0;
constexpr int kChroma   = 1;

enum Kind : int
{
	kFirst,///< the first preamp stage: its grid fed through the input stopper
	kLater,///< every later preamp stage: fed from the previous plate
	kPower,///< the power stage, single-ended or push-pull
	kKinds
};

constexpr int kRows = kChains * kChannels * kKinds;

constexpr int RowIndex( int chain, int channel, int kind )
{
	return ( chain * kChannels + channel ) * kKinds + kind;
}

/// How far each side of the centre a table reaches, volts. Past it the
/// lookup holds its last node.
constexpr double kReach = 1500.0;

/// Node i sits at v = c + w sinh( ( i - kCentre ) du ).
struct Warp
{
	double c  = 0.0;
	double w  = 1.0;
	double du = 1.0;

	double At( int i ) const;
};

/// The table's warp as the shader receives it: three floats, the third the
/// RECIPROCAL of du, since the shader multiplies.
struct WarpF
{
	float c;
	float w;
	float invDu;
};

struct Table
{
	Warp warp;
	std::vector< float > values;///< kNodes plate deviations, volts
	bool built = false;
};

struct ChainSettings
{
	int stages            = 2;
	int preamp            = model::k12AX7;
	double bias           = 0.5;///< fraction from cutoff (0) to 0 V (1)
	double drive          = 1.0;///< volts on the first grid per unit of signal
	model::PowerStage power = model::PowerStage::PushPull;
	int powerValve        = model::kEL34;
	double idle           = model::kDefaultIdle;
	double master         = 1.0;///< volts on the power grids per volt of preamp swing
};

struct Settings
{
	std::array< ChainSettings, kChains > chain;
	double interstage = 0.1;///< volts on a later grid per volt of the plate before it
	double mismatch   = 0.0;///< kG1 spread, 0..0.25
	double rest       = 0.5;///< the picture level held at the main chain's rest
	bool perChannel   = false;///< RGB with a mismatch: three different valve sets
	int perturb       = 0;
};

/// kG1 multiplier of channel c's valve set: R, G, B = 1 + m, 1, 1 - m.
double ChannelScale( int channel, double mismatch );

/// The kG1 multipliers of a push-pull pair's two halves in channel c.
double HalfScale( int channel, double mismatch, int half );

/// cos( 2 pi k / kTaps ), rounded to float as the shader receives it.
constexpr int kTaps = 64;
const std::array< float, kTaps >& CosTable();

/// The largest chroma amplitude a legal RGB colour has (red and cyan):
/// sqrt( U^2 + V^2 ) with BT.601's U = 0.492111 ( B - Y ),
/// V = 0.877283 ( R - Y ).
double ChromaReach();

/// The composite table's grid: Y at kTableY nodes over [0, 1], A at
/// kTableA over [0, ChromaReach()]; the Y/C chroma table at kTableC over
/// [0, ChromaReach()] in one extra row.
constexpr int kTableY = 256;
constexpr int kTableA = 64;
constexpr int kTableC = 256;

class Tables
{
public:
	/// Solve every table the settings need that is missing or stale. Returns
	/// a bitmask of the rows that changed (bit r for row r) for the upload.
	uint32_t Update( const Settings& settings );

	const Table& Row( int row ) const
	{
		return rows[ static_cast< size_t >( row ) ];
	}
	WarpF RowWarp( int row ) const;

	/// The first of the three rows channel `channel` of `chain` reads: its
	/// own, or channel 1's when the channels are one valve set.
	int RowBase( int chain, int channel ) const;

	/// The quiescent grid of a chain's preamp stages, and their operating
	/// point per channel.
	double Vgq( int chain ) const
	{
		return vgq[ static_cast< size_t >( chain ) ];
	}
	const model::StageQ& Quiescent( int chain, int channel ) const
	{
		return stageQ[ static_cast< size_t >( chain * kChannels + channel ) ];
	}
	const model::PowerQ& PowerQuiescent( int chain, int channel ) const
	{
		return powerQ[ static_cast< size_t >( chain * kChannels + channel ) ];
	}

	/// The chain as the shader runs it, over the float tables, in double.
	/// `s` is the signal in video units from the operating point.
	double Evaluate( int chain, int channel, double s ) const;

	/// One table's lookup as the shader does it.
	double Lookup( int row, double v ) const;

	/// The fundamental of the chain's output over one subcarrier cycle of
	/// amplitude `a` on top of `offset`, by the shader's kTaps-point rule.
	/// `dc` receives the mean.
	double Fundamental( int chain, int channel, double offset, double a, double* dc = nullptr ) const;

	/// The settings the tables were last updated with.
	const Settings& Current() const
	{
		return settings;
	}

private:
	struct Key
	{
		int valve       = -1;
		int stage       = -1;
		double bias     = -1.0;
		double scaleA   = -1.0;
		double scaleB   = -1.0;
		double source   = -1.0;
		int perturb     = -1;
		bool operator==( const Key& o ) const;
	};

	void buildPreamp( int row, const model::Valve& v, const model::StageQ& q, double source, double scale, int perturb );
	void buildPower( int row, int valve, model::PowerStage stage, const model::PowerQ& q, double scaleA, double scaleB,
	                 int perturb );

	std::array< Table, kRows > rows;
	std::array< Key, kRows > keys;
	std::array< double, kChains > vgq = { 0.0, 0.0 };
	std::array< model::StageQ, kChains * kChannels > stageQ = {};
	std::array< model::PowerQ, kChains * kChannels > powerQ = {};
	Settings settings;
};

/// Place `knee` (a voltage measured from c) on a node: the warp whose nodes
/// run to about kReach either side, with w nudged so that some node lands
/// exactly on the knee. `scale` sets how fine the grid is near the centre.
Warp AlignedWarp( double c, double scale, double knee );

//---------------------------------------------------------------------------
// What the display does with a chain's volts.
//---------------------------------------------------------------------------
enum class Output : int
{
	Fit,  ///< black to black and white to white
	Unity,///< unit small-signal gain at mid-grey
	Plate,///< the plate's own volts over the last valve's supply
	Count
};

enum class Signal : int
{
	RGB,
	YC,
	Composite,
	Count
};

/// y = offset + ( f - ref ) scale, per channel of the main chain, f the
/// chain at s = x - rest; the chroma's amplitude is F1 x chromaScale.
struct Norm
{
	double offset = 0.0;
	std::array< double, kChannels > ref   = { 0.0, 0.0, 0.0 };
	std::array< double, kChannels > scale = { 1.0, 1.0, 1.0 };
	double chromaScale = 1.0;
};

/// The chain's small-signal slope at the operating point, volts per unit of
/// signal: the mean of the slopes either side of 0 (both exact for the
/// piecewise-linear tables).
double Slope( const Tables& tables, int chain, int channel );

/// The volts Plate mode divides by: the last valve's supply.
double PlateSupply( const ChainSettings& c );

Norm Normalise( const Tables& tables, Signal signal, Output output, bool asWired );

} // namespace valvefx::chain
