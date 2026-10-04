#include "Chain.h"

#include <algorithm>
#include <cmath>

namespace valvefx::chain
{

double Warp::At( int i ) const
{
	return c + w * std::sinh( static_cast< double >( i - kCentre ) * du );
}

bool Tables::Key::operator==( const Key& o ) const
{
	return valve == o.valve && stage == o.stage && bias == o.bias && scaleA == o.scaleA && scaleB == o.scaleB
	       && source == o.source && perturb == o.perturb;
}

double ChannelScale( int channel, double mismatch )
{
	return 1.0 + mismatch * static_cast< double >( 1 - channel );
}

double HalfScale( int channel, double mismatch, int half )
{
	return ChannelScale( channel, mismatch ) * ( half == 0 ? 1.0 + mismatch : 1.0 - mismatch );
}

const std::array< float, kTaps >& CosTable()
{
	static const std::array< float, kTaps > table = [] {
		std::array< float, kTaps > t {};
		for( int k = 0; k < kTaps; ++k )
			t[ static_cast< size_t >( k ) ] = static_cast< float >( std::cos( 2.0 * model::kPi * k / kTaps ) );
		return t;
	}();
	return table;
}

double ChromaReach()
{
	const double y = 0.299;
	const double u = 0.492111 * ( 0.0 - y );
	const double v = 0.877283 * ( 1.0 - y );
	return std::sqrt( u * u + v * v );
}

Warp AlignedWarp( double c, double scale, double knee )
{
	Warp warp;
	warp.c  = c;
	warp.w  = scale;
	warp.du = std::asinh( kReach / scale ) / kCentre;
	if( !( knee > 0.0 ) )
		return warp;
	const double exact = std::asinh( knee / scale ) / warp.du;
	const int j        = std::clamp( static_cast< int >( std::lround( exact ) ), 1, kCentre );
	warp.w             = knee / std::sinh( j * warp.du );
	return warp;
}

//---------------------------------------------------------------------------
void Tables::buildPreamp( int row, const model::Valve& v, const model::StageQ& q, double source, double scale, int perturb )
{
	Table& t = rows[ static_cast< size_t >( row ) ];
	//Fine enough near rest that the 12AX7's three volts get a few thousand
	//nodes; the knee is 0 V on the grid, -eg from the centre.
	t.warp = AlignedWarp( q.eg, std::fabs( model::CutoffGrid( v, model::kPreampSupply ) ) / 8.0, -q.eg );
	//Solved outward from rest in both directions, each node's Newton
	//starting from its neighbour's plate: a few iterations each instead of
	//a few dozen from the quiescent point.
	t.values.assign( kNodes, 0.0f );
	for( const int direction : { +1, -1 } )
	{
		double plate = q.vp;
		for( int i = kCentre + direction; i >= 0 && i < kNodes; i += direction )
		{
			const double dp = model::PreampPlate( v, q, t.warp.At( i ), source, scale, perturb, plate );
			plate           = q.vp + dp;
			t.values[ static_cast< size_t >( i ) ] = static_cast< float >( dp );
		}
	}
	t.built = true;
}

void Tables::buildPower( int row, int valve, model::PowerStage stage, const model::PowerQ& q, double scaleA, double scaleB,
                         int perturb )
{
	Table& t = rows[ static_cast< size_t >( row ) ];
	//The drive is measured from rest, so the centre is 0 V of drive; each
	//grid reaches 0 V at a drive of |bias| (both of them, by symmetry, for a
	//push-pull pair: the warp is odd about the centre).
	const double knee = std::fabs( q.bias );
	t.warp            = AlignedWarp( 0.0, std::max( knee, 1.0 ) / 8.0, knee );
	t.values.assign( kNodes, 0.0f );
	for( const int direction : { +1, -1 } )
	{
		double unknown = stage == model::PowerStage::SingleEnded ? model::PowerCircuitOf( valve ).bplus : q.vhq;
		for( int i = kCentre + direction; i >= 0 && i < kNodes; i += direction )
			t.values[ static_cast< size_t >( i ) ] =
				static_cast< float >( model::PowerOut( valve, stage, q, t.warp.At( i ), scaleA, scaleB, perturb, &unknown ) );
	}
	t.built = true;
}

uint32_t Tables::Update( const Settings& next )
{
	settings      = next;
	uint32_t mask = 0;

	for( int chain = 0; chain < kChains; ++chain )
	{
		const ChainSettings& cs = next.chain[ static_cast< size_t >( chain ) ];
		const model::Valve& v   = model::PreampValve( cs.preamp, next.perturb );
		vgq[ static_cast< size_t >( chain ) ] = model::PreampBiasGrid( v, cs.bias );

		const bool split = chain == kMain && next.perChannel;
		for( int ch = split ? 0 : 1; ch <= ( split ? 2 : 1 ); ++ch )
		{
			const size_t at = static_cast< size_t >( chain * kChannels + ch );
			const double scale = ChannelScale( ch, next.mismatch );
			stageQ[ at ]       = model::PreampQuiescent( v, vgq[ static_cast< size_t >( chain ) ], scale );
			const int stagePerturb = next.perturb & ( model::kPerturbNoGridCurrent | model::kPerturbWrongMu );

			auto preamp = [ & ]( int kind, double source ) {
				const int row = RowIndex( chain, ch, kind );
				Key key;
				key.valve   = cs.preamp;
				key.bias    = vgq[ static_cast< size_t >( chain ) ];
				key.scaleA  = scale;
				key.source  = source;
				key.perturb = stagePerturb;
				if( rows[ static_cast< size_t >( row ) ].built && keys[ static_cast< size_t >( row ) ] == key )
					return;
				buildPreamp( row, v, stageQ[ at ], source, scale, next.perturb );
				keys[ static_cast< size_t >( row ) ] = key;
				mask |= 1u << row;
			};
			if( cs.stages >= 1 )
				preamp( kFirst, model::kInputStopper );
			if( cs.stages >= 2 )
				preamp( kLater, stageQ[ at ].rout );

			if( cs.power != model::PowerStage::Off )
			{
				const bool pp       = cs.power == model::PowerStage::PushPull;
				const double sA     = pp ? HalfScale( ch, next.mismatch, 0 ) : scale;
				const double sB     = pp ? HalfScale( ch, next.mismatch, 1 ) : scale;
				const int powerBits = next.perturb
				                      & ( model::kPerturbNoGridCurrent | model::kPerturbUndrivenHalf | model::kPerturbSplitLoadLine );
				powerQ[ at ] = model::PowerQuiescent( cs.powerValve, cs.power, cs.idle, sA, sB, powerBits );

				const int row = RowIndex( chain, ch, kPower );
				Key key;
				key.valve   = cs.powerValve;
				key.stage   = static_cast< int >( cs.power );
				key.bias    = cs.idle;
				key.scaleA  = sA;
				key.scaleB  = sB;
				key.perturb = powerBits;
				if( !( rows[ static_cast< size_t >( row ) ].built && keys[ static_cast< size_t >( row ) ] == key ) )
				{
					buildPower( row, cs.powerValve, cs.power, powerQ[ at ], sA, sB, powerBits );
					keys[ static_cast< size_t >( row ) ] = key;
					mask |= 1u << row;
				}
			}
		}
	}
	return mask;
}

WarpF Tables::RowWarp( int row ) const
{
	const Warp& w = rows[ static_cast< size_t >( row ) ].warp;
	return { static_cast< float >( w.c ), static_cast< float >( w.w ), static_cast< float >( 1.0 / w.du ) };
}

int Tables::RowBase( int chain, int channel ) const
{
	const bool split = chain == kMain && settings.perChannel;
	return RowIndex( chain, split ? channel : 1, 0 );
}

double Tables::Lookup( int row, double v ) const
{
	const Table& t = rows[ static_cast< size_t >( row ) ];
	if( !t.built )
		return 0.0;
	const WarpF wf = RowWarp( row );
	//The shader's asinh, written out: sign( z ) log( |z| + sqrt( z^2 + 1 ) ).
	const double z = ( v - static_cast< double >( wf.c ) ) / static_cast< double >( wf.w );
	const double a = std::fabs( z );
	const double u = std::copysign( std::log( a + std::sqrt( a * a + 1.0 ) ), z );
	double x       = kCentre + u * static_cast< double >( wf.invDu );
	x              = std::clamp( x, 0.0, static_cast< double >( kNodes - 1 ) );
	const int i    = std::min( static_cast< int >( x ), kNodes - 2 );
	const double f = x - i;
	const double l = t.values[ static_cast< size_t >( i ) ];
	const double r = t.values[ static_cast< size_t >( i + 1 ) ];
	return l + f * ( r - l );
}

double Tables::Evaluate( int chain, int channel, double s ) const
{
	const ChainSettings& cs = settings.chain[ static_cast< size_t >( chain ) ];
	const int base          = RowBase( chain, channel );
	//Every number the shader is handed is a float: round them the same way.
	const double vg    = static_cast< float >( vgq[ static_cast< size_t >( chain ) ] );
	const double drive = static_cast< float >( cs.drive );
	const double gain  = static_cast< float >( cs.master );
	const double k     = static_cast< float >( settings.interstage );

	double d = 0.0;
	if( cs.stages > 0 )
	{
		double p = Lookup( base + kFirst, vg + drive * s );
		for( int i = 1; i < cs.stages; ++i )
			p = Lookup( base + kLater, vg + k * p );
		if( cs.power == model::PowerStage::Off )
			return p;
		d = gain * p;
	}
	else
	{
		if( cs.power == model::PowerStage::Off )
			return drive * s;
		d = drive * s;
	}
	return Lookup( base + kPower, d );
}

double Tables::Fundamental( int chain, int channel, double offset, double a, double* dc ) const
{
	const auto& cosines = CosTable();
	double sum = 0.0, first = 0.0;
	for( int k = 0; k < kTaps; ++k )
	{
		const double c   = cosines[ static_cast< size_t >( k ) ];
		const double out = Evaluate( chain, channel, offset + a * c );
		sum += out;
		first += out * c;
	}
	if( dc )
		*dc = sum / kTaps;
	return first * 2.0 / kTaps;
}

//---------------------------------------------------------------------------
double Slope( const Tables& tables, int chain, int channel )
{
	//Far inside the centre node's two segments in every stage, so each side
	//is the exact slope of the piecewise-linear chain on that side.
	const ChainSettings& cs = tables.Current().chain[ static_cast< size_t >( chain ) ];
	const double h          = 1e-9 / std::max( 1.0, cs.drive );
	const double f0         = tables.Evaluate( chain, channel, 0.0 );
	const double right      = ( tables.Evaluate( chain, channel, h ) - f0 ) / h;
	const double left       = ( f0 - tables.Evaluate( chain, channel, -h ) ) / h;
	return 0.5 * ( left + right );
}

double PlateSupply( const ChainSettings& c )
{
	return c.power != model::PowerStage::Off ? model::PowerSupply( c.powerValve ) : model::kPreampSupply;
}

namespace
{
double inverse( double x )
{
	return std::fabs( x ) > 1e-300 ? 1.0 / x : 0.0;
}

double sign( double x )
{
	return x < 0.0 ? -1.0 : 1.0;
}
} // namespace

Norm Normalise( const Tables& tables, Signal signal, Output output, bool asWired )
{
	Norm n;
	const Settings& s = tables.Current();
	for( int ch = 0; ch < kChannels; ++ch )
	{
		const int channel = signal == Signal::RGB ? ch : 1;
		const double f0   = tables.Evaluate( kMain, channel, -s.rest );
		const double f1   = tables.Evaluate( kMain, channel, 1.0 - s.rest );
		const double fs   = Slope( tables, kMain, channel );
		const size_t c    = static_cast< size_t >( ch );
		switch( output )
		{
		case Output::Fit:
		case Output::Count:
			n.offset     = 0.0;
			n.ref[ c ]   = asWired ? std::min( f0, f1 ) : f0;
			n.scale[ c ] = asWired ? inverse( std::fabs( f1 - f0 ) ) : inverse( f1 - f0 );
			break;
		case Output::Unity:
			n.offset     = s.rest;
			n.ref[ c ]   = tables.Evaluate( kMain, channel, 0.0 );
			n.scale[ c ] = asWired ? inverse( std::fabs( fs ) ) : inverse( fs );
			break;
		case Output::Plate:
			n.offset     = s.rest;
			n.ref[ c ]   = 0.0;
			n.scale[ c ] = ( asWired ? 1.0 : sign( fs ) ) / PlateSupply( s.chain[ kMain ] );
			break;
		}
	}

	if( signal != Signal::YC )
	{
		//Composite is one signal: the chroma rides the luma's scale.
		n.chromaScale = n.scale[ 1 ];
		return n;
	}
	switch( output )
	{
	case Output::Fit:
	case Output::Count:
	{
		const double reach = ChromaReach();
		const double f     = tables.Fundamental( kChroma, 1, 0.0, reach );
		n.chromaScale      = reach * ( asWired ? inverse( std::fabs( f ) ) : inverse( f ) );
		break;
	}
	case Output::Unity:
	{
		const double gs = Slope( tables, kChroma, 1 );
		n.chromaScale   = asWired ? inverse( std::fabs( gs ) ) : inverse( gs );
		break;
	}
	case Output::Plate:
		n.chromaScale = ( asWired ? 1.0 : sign( Slope( tables, kChroma, 1 ) ) ) / PlateSupply( s.chain[ kChroma ] );
		break;
	}
	return n;
}

} // namespace valvefx::chain
