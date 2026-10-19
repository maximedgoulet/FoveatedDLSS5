// The HDR commit under Feed v2 (NRFeedMode): the dark gate and the pedestal
// cap are in NR-input units, scaled by the input scale the commit sets carry
// at t2.  v1 keeps v6_commit and its source-unit gate unchanged.
#define GATE_IN_INPUT_UNITS
#include "v6_commit.cs_5_1.hlsl"
