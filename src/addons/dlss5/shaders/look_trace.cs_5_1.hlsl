#include "look_common.hlsli"

// NREditTrace (group I): observe-only statistics of the RAW network edit,
// bound with the pass's codec set (Neural = the untouched NR output), so it
// measures what the model did whether or not the look stage then reshapes it.
//   Mode 0: clear TraceHistogram (one group).
//   Mode 1: one 16x16 group per 32x32 block of the NR grid:
//     TraceHistogram[0..127]  count of a in [-4, 4) at 1/16 stop (clamped)
//     TraceHistogram[128]     pixels counted
//     TraceHistogram[129]     |a| > 1 stop
//     TraceHistogram[130]     halo: more than 2 stops below the block's
//                             brightest input and darkened by over 0.1 stop
//     TraceBlocks[block]      mean a, mean I, mean |c_r|_w, mean |c_t|_w
// edit_trace.hpp reads both back; look_reference.py (trace) mirrors them.

static const uint kTraceBins = 128;
static const uint kTraceWords = kTraceBins + 4;
static const float kTraceLow = -4.0;
static const float kTraceStep = 1.0 / 16.0;

groupshared uint g_histogram[kTraceWords];
groupshared uint g_peak;
groupshared float4 g_sum[256];

// Order-preserving float -> uint, so InterlockedMax finds the float maximum.
uint OrderedFloat(float value) {
  const uint bits = asuint(value);
  return (bits & 0x80000000u) != 0u ? ~bits : bits | 0x80000000u;
}

float UnorderedFloat(uint bits) {
  return asfloat((bits & 0x80000000u) != 0u ? bits & 0x7FFFFFFFu : ~bits);
}

[numthreads(16, 16, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
          uint index : SV_GroupIndex) {
  if (Mode == 0u) {
    if (index < kTraceWords) TraceHistogram.Store(index * 4u, 0u);
    return;
  }
  if (index < kTraceWords) g_histogram[index] = 0u;
  if (index == 0u) g_peak = 0u;
  GroupMemoryBarrierWithGroupSync();

  const int2 origin = int2(group.xy) * 32 + int2(thread.xy) * 2;
  float I[4];
  float a[4];
  bool valid[4];
  float4 sum = 0.0;
  [unroll]
  for (uint k = 0; k < 4u; ++k) {
    const int2 pixel = origin + int2(k & 1u, k >> 1);
    valid[k] = all(pixel < int2(NrSize));
    I[k] = 0.0;
    a[k] = 0.0;
    if (valid[k]) {
      const Edit x = ReadEdit(pixel);
      float3 c_r;
      float3 c_t;
      float s;
      ColourSplit(x.c, x.lp, c_r, c_t, s);
      I[k] = x.I;
      a[k] = x.a;
      const uint bin = uint(clamp(floor((x.a - kTraceLow) / kTraceStep), 0.0,
                                  float(kTraceBins - 1u)));
      InterlockedAdd(g_histogram[bin], 1u);
      InterlockedAdd(g_histogram[kTraceBins], 1u);
      if (abs(x.a) > 1.0) InterlockedAdd(g_histogram[kTraceBins + 1u], 1u);
      InterlockedMax(g_peak, OrderedFloat(x.I));
      sum += float4(x.a, x.I, sqrt(dot(kW, c_r * c_r)), sqrt(dot(kW, c_t * c_t)));
    }
  }
  g_sum[index] = sum;
  GroupMemoryBarrierWithGroupSync();

  const float peak = UnorderedFloat(g_peak);
  [unroll]
  for (uint j = 0; j < 4u; ++j) {
    if (valid[j] && peak - I[j] > 2.0 && a[j] < -0.1) {
      InterlockedAdd(g_histogram[kTraceBins + 2u], 1u);
    }
  }
  for (uint stride = 128u; stride > 0u; stride >>= 1) {
    if (index < stride) g_sum[index] += g_sum[index + stride];
    GroupMemoryBarrierWithGroupSync();
  }
  if (index == 0u) {
    const uint count = g_histogram[kTraceBins];
    TraceBlocks[group.xy] = count != 0u ? g_sum[0] / float(count) : 0.0;
  }
  if (index < kTraceWords && g_histogram[index] != 0u) {
    uint previous;
    TraceHistogram.InterlockedAdd(index * 4u, g_histogram[index], previous);
  }
}
