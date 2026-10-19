// The black-level restore's reduce while the look shapes the resolve: it
// measures NR's own lift from the unshaped model output, so the restore
// takes back NR's pedestal and leaves the look's change alone.
#define MEASURE_UNSHAPED
#include "legacy_reduce.cs_5_1.hlsl"
