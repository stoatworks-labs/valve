/*
 * Records every libm call the plugin's C++ makes, for the cross-check's replay.
 *
 *     DYLD_INSERT_LIBRARIES=librecord.dylib LIBM_RECORD=calls.bin ./printer
 *
 * Why this exists: JavaScript's Math.exp, Math.log1p, Math.pow and friends are
 * V8's own (fdlibm's algorithms), the plugin's std::exp and friends are Apple's
 * libm, and the two disagree in the last bit of a few results in a hundred.
 * Neither is wrong -- neither promises correct rounding -- but a port compared
 * through them can only be compared to within a tolerance, and a tolerance
 * hides the small port errors as well as the libm's. So the cross-check runs
 * the C++ with this interposed (dyld's __interpose section: every call from the
 * printer's code to libm comes here first), writes down each call's argument
 * and result bits, and run.mjs answers the port's Math calls from that record.
 * With the libm taken out, the port's arithmetic must match the plugin's to the
 * bit; crosscheck.sh reports the libm's own differences separately.
 *
 * Valve's libm: exp, log1p and pow in Koren's law, atan in the pentode's knee,
 * sinh and asinh in the tables' warp, log in the lookup's written-out asinh,
 * cos in the subcarrier's table, log10 in the constructor's defaults -- and
 * __exp10, because clang turns std::pow( 10.0, x ) into a call of it on Darwin
 * (the control laws). Adapted from slipstream's recorder, which covers a
 * different set.
 *
 * Record format, little-endian: one byte the function, then its argument(s)
 * and result as raw IEEE bits (8 bytes a double).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern double __exp10( double );

static FILE* out;
static int opened;

static void put( uint8_t fn, const void* data, size_t size )
{
	if( !opened )
	{
		opened         = 1;
		const char* to = getenv( "LIBM_RECORD" );
		out            = to ? fopen( to, "wb" ) : NULL;
	}
	if( !out )
		return;
	fputc( fn, out );
	fwrite( data, 1, size, out );
}

#define ONE( id, name )                                \
	static double record_##name( double x )            \
	{                                                  \
		double r = name( x );                          \
		double buf[ 2 ] = { x, r };                    \
		put( id, buf, sizeof buf );                    \
		return r;                                      \
	}

ONE( 1, exp )
ONE( 2, log1p )
ONE( 3, log )
ONE( 4, log10 )
ONE( 5, sinh )
ONE( 6, asinh )
ONE( 7, atan )
ONE( 8, cos )
ONE( 9, __exp10 )
ONE( 10, sin )

static double record_pow( double x, double y )
{
	double r = pow( x, y );
	double buf[ 3 ] = { x, y, r };
	put( 11, buf, sizeof buf );
	return r;
}

__attribute__( ( destructor ) ) static void finish( void )
{
	if( out )
		fclose( out );
	out = NULL;
}

#define INTERPOSE( replacement, replacee ) \
	{ (const void*)(unsigned long)&replacement, (const void*)(unsigned long)&replacee }

__attribute__( ( used ) ) static const struct
{
	const void* replacement;
	const void* replacee;
} interposers[] __attribute__( ( section( "__DATA,__interpose" ) ) ) = {
	INTERPOSE( record_exp, exp ),     INTERPOSE( record_log1p, log1p ), INTERPOSE( record_log, log ),
	INTERPOSE( record_log10, log10 ), INTERPOSE( record_sinh, sinh ),   INTERPOSE( record_asinh, asinh ),
	INTERPOSE( record_atan, atan ),   INTERPOSE( record_cos, cos ),     INTERPOSE( record___exp10, __exp10 ),
	INTERPOSE( record_sin, sin ),     INTERPOSE( record_pow, pow ),
};
