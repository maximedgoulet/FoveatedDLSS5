// The HDR pedestal reduce while the look shapes a single-pass resolve: it
// measures NR's own lift from the unshaped model output, so the commit takes
// back NR's pedestal and leaves the look's change alone.
#define MEASURE_UNSHAPED
#include "v6_pedestal_reduce.cs_5_1.hlsl"
