#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace valvefx::controls
{
namespace
{
double unit( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}
} // namespace

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

int Stages( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, kMaxStages );
}

double DriveVolts( float value )
{
	return std::pow( 10.0, -2.0 + 4.5 * unit( value ) );
}

double MasterGain( float value )
{
	return std::pow( 10.0, -2.0 + 4.0 * unit( value ) );
}

double BiasFraction( float value )
{
	return 0.05 + 0.9 * unit( value );
}

double IdleFraction( float value )
{
	return 0.05 + 0.95 * unit( value );
}

double InterstageGain( float value )
{
	return std::pow( 10.0, ( -40.0 + 40.0 * unit( value ) ) / 20.0 );
}

double RestLevel( float value )
{
	return unit( value );
}

double MismatchFraction( float value )
{
	return 0.25 * unit( value );
}

float DriveSlider( double volts )
{
	return static_cast< float >( ( std::log10( volts ) + 2.0 ) / 4.5 );
}

float MasterSlider( double gain )
{
	return static_cast< float >( ( std::log10( gain ) + 2.0 ) / 4.0 );
}

float IdleSlider( double fraction )
{
	return static_cast< float >( ( fraction - 0.05 ) / 0.95 );
}

} // namespace valvefx::controls
