#pragma once

#include <string>

/**
	The passes. Every read is `texelFetch` at integer coordinates computed in
	integers and every interpolation is written out in float, so nothing here
	depends on a texture unit's filtering. Every table and coefficient is
	computed on the CPU in double (`Valves.cpp`, `Chain.cpp`) and handed over
	as floats; the GPU looks up, multiplies and adds. There is no sin, cos or
	atan in the GLSL: the subcarrier's cosines arrive as a table.

	  library  (no #version, no main) the chain: each stage a lookup in its
	           table on the asinh-warped grid, the interstage loss, Master,
	           the power stage. Spliced into both passes below, so the table
	           pass and the display run the SAME text -- and vatest dumps the
	           assembled sources, not a transcription.
	  tables   the describing functions, 256 x 65: for Composite the mean and
	           the fundamental of the chain over one subcarrier cycle on a
	           ( Y, A ) grid; in the last row, for Y/C, the chroma chain's
	           fundamental against amplitude. Only when Signal needs it.
	  display  un-premultiply, the mode, the normalisation, back to RGB,
	           re-premultiply, Mix; and Show Curve's box.
*/
namespace valvefx::shaders
{

extern const char* const kVertex;

/// The assembled fragment shaders, exactly as the plugin compiles them.
const std::string& TablesSource();
const std::string& DisplaySource();

} // namespace valvefx::shaders
