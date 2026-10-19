/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// One-shot NR screenshot pair (pre-NR DLSS output + final NR output).
//
// Design contract (v2.7 rewrite): the feature must be inert while idle. The
// present path never touches D3D12 objects, user32 (beyond the one hotkey
// poll), std::filesystem, or the heap on behalf of this feature until the user
// arms it. Concretely:
//   - No QueryInterface and no command queue anywhere in this file.  GPU
//     completion is proven with a fence-completed lease (gpu_lease.hpp)
//     acquired once the capture frame has presented - a poll of
//     ID3D12Fence::GetCompletedValue, never a CPU wait - with the
//     present-generation delay (kSettlePresents) kept as the fallback for
//     queues without a tracked fence.
//   - The render/evaluate thread only creates two addon-owned READBACK heap
//     buffers (Prepare stage). No filesystem calls there either.
//   - Readback mapping, RGBA conversion, PNG encoding, and file IO run on a
//     dedicated worker thread (created lazily on the first hand-off). The
//     present thread only moves a finished capture into the worker's job slot.
//   - runtime_mutex (dlssnr.hpp) is never held across any of that work. This
//     module has its own state_mutex; the only legal ordering is
//     runtime_mutex -> state_mutex -> job_mutex, and the worker takes
//     job_mutex alone.
//   - The worker is detached and never joined: Shutdown runs under the DLL
//     loader lock (DllMain PROCESS_DETACH), where joining any running thread
//     deadlocks. Shutdown drains owned state and raises shutdown_requested;
//     the worker exits on its own and the process reclaims it at exit.
//
// Compatibility notes (v4.1.5): the PNG is always written with opaque alpha -
// HDR intermediates routinely carry alpha 0/NaN, and an alpha-respecting
// viewer composites those to a fully black image.  The pair may be cropped to
// the engine-declared output subrect when the DLSS output resource is larger
// than what the engine actually renders (black-margin titles).

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <include/reshade.hpp>

#include "../../utils/path.hpp"
#include "../../utils/png.hpp"
#include "gpu_lease.hpp"

namespace renodx::addons::dlss5::screenshot {
enum class FileFormat : uint32_t { kPng = 0, kJpeg = 1, kPngAndJpeg = 2 };
inline std::atomic_uint32_t file_format = 0;
inline std::atomic_uint32_t layout = 0;
inline std::atomic_uint32_t jpeg_quality = 95;
inline std::atomic_uint32_t max_megabytes = 10;

namespace internal {

// Fallback for queues without a tracked completion fence: presents that must
// pass after the copies were recorded before the readback buffers are
// mapped. Matches the retire-settle window the resource-release path uses;
// with fences the lease replaces this age rule.
constexpr uint64_t kSettlePresents = 4;
// Presents an armed capture may wait without a single DLSS evaluation before
// it disarms itself (~10 s at 60 fps).  Without this, F5 pressed during a
// loading screen or with NR off fires much later on the first evaluate -
// typically a black frame the user cannot make sense of.
constexpr uint32_t kArmTimeoutPresents = 600;
constexpr char kLogPrefix[] = "DLSS5 Generic";

// Raw float32 RGBA diagnostic planes captured alongside the PNG pair:
// the sRGB proxy the NR model actually sees, and the resolved linear work
// surface before the commit.  PNGs cannot establish exposure/units; these
// dumps (plus the sidecar meta) can.
struct DiagnosticPlane {
  std::string name;
  ID3D12Resource* readback = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total_bytes = 0;
};

struct OutputOptions {
  FileFormat format = FileFormat::kPng;
  bool side_by_side = false;
  uint32_t jpeg_quality = 95;
  uint32_t max_megabytes = 10;
};

inline OutputOptions requested_output;

struct PendingCapture {
  OutputOptions output;
  ID3D12Resource* pre_nr_readback = nullptr;
  ID3D12Resource* nr_output_readback = nullptr;
  std::vector<DiagnosticPlane> planes;
  std::string meta;  // sidecar .meta.txt content (divisor, encoding, meter)
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total_bytes = 0;
  UINT row_pitch = 0;
  // Captured subrect: engines can render into a subrect of a larger DLSS
  // output resource; width/height are the CROP dimensions written to the PNG,
  // while the footprint/row_pitch still describe the full resource rows.
  uint32_t crop_x = 0;
  uint32_t crop_y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint8_t hdr_mode = 0;  // 0 = SDR, 1 = linear HDR, 2 = PQ HDR
  uint64_t timestamp_ms = 0;  // filename stem components; paths are only built
  uint64_t serial = 0;        // on the worker so Prepare never touches the fs
  uint64_t creation_generation = 0;  // present_generation when recorded
  // Acquired at the capture frame's present (all submits carrying the copies
  // are queued by then); empty until then or when no queue fence exists.
  // lease_generation stamps when the lease was taken so a fence that never
  // advances (present-starved session) settles on the bounded fallback.
  GpuLease lease;
  uint64_t lease_generation = 0;
  bool lease_taken = false;
  bool ready = false;                // both copies have been recorded
  bool aborted = false;
};

inline std::atomic_bool capture_requested = false;
inline std::atomic_uint32_t capture_serial = 0;
inline std::atomic_bool shutdown_requested = false;
inline std::atomic_bool hotkey_was_down = false;
// Presents counted while armed without any evaluate having recorded a pair.
inline std::atomic_uint32_t presents_while_armed = 0;

// pending is guarded by state_mutex; has_pending mirrors pending.has_value()
// so the idle present path stays a single atomic load.
inline std::mutex state_mutex;
inline std::optional<PendingCapture> pending;
inline std::atomic_bool has_pending = false;

struct FinishedCapture {
  uint64_t serial = 0;
  const char* outcome = "none";
  bool model_diagnostic = false;
  // The pair's lossless files when PNG was written, else its JPEGs.
  std::filesystem::path pre_image, post_image, meta;  // empty when not written
};
inline std::mutex finished_mutex;
inline FinishedCapture finished;
inline std::atomic_uint64_t finished_serial = 0;

inline void PublishFinished(FinishedCapture record) noexcept {
  try {
    std::scoped_lock lock(finished_mutex);
    const uint64_t serial = record.serial;
    if (serial < finished.serial) return;
    finished = std::move(record);
    finished_serial.store(serial, std::memory_order_release);
  } catch (...) {
  }
}

// Buffers from evaluates that were superseded within the same capture-frame
// (the capture re-records on every DLSS-family evaluate so the LAST one wins).
// They share the frame's command list, so they are released at the same settle
// window as the final capture rather than immediately.  This owns the ENTIRE
// superseded buffer set - the PNG pair AND every diagnostic plane: replacing
// `pending` used to destroy the old planes vector without releasing its raw
// COM readback pointers, leaking every re-recorded plane (a 4K FP16 plane is
// ~63 MiB; v6 audit issue 13).
struct RetiredCaptureBuffers {
  ID3D12Resource* pre_nr_readback = nullptr;
  ID3D12Resource* nr_output_readback = nullptr;
  std::vector<DiagnosticPlane> planes;

  void Release() {
    if (pre_nr_readback != nullptr) pre_nr_readback->Release();
    if (nr_output_readback != nullptr) nr_output_readback->Release();
    for (DiagnosticPlane& plane : planes) {
      if (plane.readback != nullptr) plane.readback->Release();
    }
    pre_nr_readback = nullptr;
    nr_output_readback = nullptr;
    planes.clear();
  }

  // Takes ownership of a cancelled PendingCapture's raw buffers for
  // retention (unproven GPU completion: release happens behind the next
  // proven settle or at teardown, never on a timeout).
  void Take(PendingCapture&& capture) {
    pre_nr_readback = capture.pre_nr_readback;
    nr_output_readback = capture.nr_output_readback;
    planes = std::move(capture.planes);
    capture.pre_nr_readback = nullptr;
    capture.nr_output_readback = nullptr;
    capture.planes.clear();
  }
};
inline std::vector<RetiredCaptureBuffers> retired_buffers;

// Worker hand-off slot. Guarded by job_mutex; the worker moves the job out
// under the lock and processes it outside.
inline std::mutex job_mutex;
inline std::condition_variable job_signal;
inline std::optional<PendingCapture> job;
inline std::atomic_bool worker_started = false;

inline void SafeLog(reshade::log::level level, const std::string& message) {
  if (shutdown_requested.load(std::memory_order_acquire)) return;
  reshade::log::message(level, (std::string(kLogPrefix) + ": " + message).c_str());
}

// std::filesystem::path::string() throws for characters the ANSI code page
// cannot represent; logging must never throw.
inline std::string PathToLogString(const std::filesystem::path& path) {
  try {
    return path.string();
  } catch (...) {
  }
  try {
    const std::wstring wide = path.wstring();
    std::string lossy;
    lossy.reserve(wide.size());
    for (wchar_t c : wide) lossy.push_back(static_cast<char>(c));
    return lossy;
  } catch (...) {
    return "<unprintable path>";
  }
}

inline void ReleaseReadbacks(PendingCapture& capture) {
  if (capture.pre_nr_readback != nullptr) capture.pre_nr_readback->Release();
  if (capture.nr_output_readback != nullptr) capture.nr_output_readback->Release();
  capture.pre_nr_readback = nullptr;
  capture.nr_output_readback = nullptr;
  for (DiagnosticPlane& plane : capture.planes) {
    if (plane.readback != nullptr) plane.readback->Release();
  }
  capture.planes.clear();
}

// Shared readback-buffer allocator for the PNG pair and the float planes.
inline bool MakeReadbackBuffer(
    ID3D12Device* device,
    UINT64 bytes,
    ID3D12Resource** result) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = bytes;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, IID_PPV_ARGS(result)));
}

// ---------------------------------------------------------------------------
// PNG encoding. Stored DEFLATE blocks are deliberate: captures are one-shot
// diagnostics, so simplicity and exact byte ownership beat compression ratio.

inline void AppendPngBe32(std::vector<uint8_t>& bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  bytes.push_back(static_cast<uint8_t>(value & 0xff));
}

inline uint32_t PngCrc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint32_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (-(crc & 1u)));
  }
  return ~crc;
}

inline uint32_t PngAdler32(const uint8_t* data, size_t size) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (size_t i = 0; i < size; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

inline void AppendPngChunk(
    std::vector<uint8_t>& png,
    const char type[4],
    const std::vector<uint8_t>& payload) {
  AppendPngBe32(png, static_cast<uint32_t>(payload.size()));
  const size_t type_offset = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), payload.begin(), payload.end());
  AppendPngBe32(png, PngCrc32(png.data() + type_offset, 4 + payload.size()));
}

inline bool WicEncode(
    IWICImagingFactory* factory, bool jpeg, uint32_t width, uint32_t height,
    const void* pixels, bool sixteen, float quality, std::vector<uint8_t>* out,
    bool* chroma_444 = nullptr) {
  using Microsoft::WRL::ComPtr;
  using CreateStreamFn = HRESULT(WINAPI*)(HGLOBAL, BOOL, LPSTREAM*);
  static const auto create_stream =
      renodx::utils::png::internal::LoadModuleProc<CreateStreamFn>(L"ole32.dll", "CreateStreamOnHGlobal");
  // wincodec.h's GUIDs, spelled out: their definitions live in a library
  // this addon does not link (as utils/png.hpp does).
  static constexpr GUID kJpegContainer = {0x19e4a5aa, 0x5662, 0x4fc5, {0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57}};
  static constexpr GUID kPngContainer = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
  static constexpr GUID kBgr24 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};
  static constexpr GUID kRgb48 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x15}};
  if (factory == nullptr || create_stream == nullptr || (jpeg && sixteen)) return false;
  ComPtr<IStream> stream;
  if (FAILED(create_stream(nullptr, TRUE, &stream))) return false;
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(jpeg ? kJpegContainer : kPngContainer, nullptr, &encoder))
      || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
    return false;
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> options;
  if (FAILED(encoder->CreateNewFrame(&frame, &options))) return false;
  PROPBAG2 option{};
  VARIANT value{};
  if (jpeg) {
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    value.vt = VT_R4;
    value.fltVal = quality;
    if (FAILED(options->Write(1, &option, &value))) return false;
    option.pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
    value.vt = VT_UI1;
    value.bVal = 3;  // WICJpegYCrCbSubsampling444 (Windows 8.1+)
    const bool full_chroma = SUCCEEDED(options->Write(1, &option, &value));
    if (chroma_444 != nullptr) *chroma_444 = full_chroma;
  } else {
    option.pstrName = const_cast<LPOLESTR>(L"FilterOption");
    value.vt = VT_UI1;
    value.bVal = 6;  // WICPngFilterAdaptive; the encoder's default otherwise
    options->Write(1, &option, &value);
  }
  const GUID wanted = sixteen ? kRgb48 : kBgr24;
  WICPixelFormatGUID format = wanted;
  const UINT stride = width * (sixteen ? 6u : 3u);
  if (FAILED(frame->Initialize(options.Get())) || FAILED(frame->SetSize(width, height))
      || FAILED(frame->SetPixelFormat(&format)) || !InlineIsEqualGUID(format, wanted)
      || FAILED(frame->WritePixels(height, stride, stride * height,
                                   static_cast<BYTE*>(const_cast<void*>(pixels))))
      || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
    return false;
  }
  STATSTG stat{};
  const LARGE_INTEGER start{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0
      || stat.cbSize.QuadPart > 0x7fffffffull
      || FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) {
    return false;
  }
  out->resize(static_cast<size_t>(stat.cbSize.QuadPart));
  ULONG read = 0;
  return SUCCEEDED(stream->Read(out->data(), static_cast<ULONG>(out->size()), &read))
         && read == out->size();
}

inline bool WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return file.good();
}

inline std::vector<uint8_t> EncodePngRgba8Stored(
    uint32_t width,
    uint32_t height,
    const std::vector<uint8_t>& rgba) {
  if (width == 0 || height == 0
      || rgba.size() != static_cast<size_t>(width) * height * 4)
    return {};

  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (1u + width * 4u));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);  // PNG filter: none
    const auto* row = rgba.data() + static_cast<size_t>(y) * width * 4u;
    raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 4u);
  }

  std::vector<uint8_t> zlib = {0x78, 0x01};
  size_t offset = 0;
  while (offset < raw.size()) {
    const size_t block_size = std::min<size_t>(65535, raw.size() - offset);
    const bool final_block = offset + block_size == raw.size();
    zlib.push_back(final_block ? 1u : 0u);
    const uint16_t length = static_cast<uint16_t>(block_size);
    const uint16_t inverse = static_cast<uint16_t>(~length);
    zlib.push_back(static_cast<uint8_t>(length & 0xff));
    zlib.push_back(static_cast<uint8_t>(length >> 8));
    zlib.push_back(static_cast<uint8_t>(inverse & 0xff));
    zlib.push_back(static_cast<uint8_t>(inverse >> 8));
    zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + block_size);
    offset += block_size;
  }
  AppendPngBe32(zlib, PngAdler32(raw.data(), raw.size()));

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  std::vector<uint8_t> ihdr;
  AppendPngBe32(ihdr, width);
  AppendPngBe32(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // RGBA8, no interlace
  AppendPngChunk(png, "IHDR", ihdr);
  AppendPngChunk(png, "IDAT", zlib);
  AppendPngChunk(png, "IEND", {});

  return png;
}

inline bool WritePngRgba8(const std::filesystem::path& path, uint32_t width,
                          uint32_t height, const std::vector<uint8_t>& rgba) {
  if (width == 0 || height == 0 || rgba.size() != static_cast<size_t>(width) * height * 4u) return false;
  std::vector<uint8_t> bgr(rgba.size() / 4u * 3u);
  for (size_t pixel = 0; pixel < rgba.size() / 4u; ++pixel) {
    bgr[pixel * 3u] = rgba[pixel * 4u + 2u];
    bgr[pixel * 3u + 1u] = rgba[pixel * 4u + 1u];
    bgr[pixel * 3u + 2u] = rgba[pixel * 4u];
  }
  renodx::utils::png::internal::ScopedComInitialization com;
  const auto factory = com.IsUsable() ? renodx::utils::png::internal::CreateWicFactory() : nullptr;
  std::vector<uint8_t> png;
  if (!WicEncode(factory.Get(), false, width, height, bgr.data(), false, 1.f, &png)) {
    png = EncodePngRgba8Stored(width, height, rgba);
  }
  return WriteBytes(path, png);
}

// ---------------------------------------------------------------------------
// Readback decoding.

inline float HalfToFloat(uint16_t bits) {
  const uint32_t sign = (bits & 0x8000u) << 16;
  const uint32_t exponent = (bits >> 10) & 0x1fu;
  const uint32_t mantissa = bits & 0x3ffu;
  uint32_t value = 0;
  if (exponent == 0) {
    if (mantissa != 0) {
      float result = std::ldexp(static_cast<float>(mantissa), -24);
      return (sign != 0 ? -result : result);
    }
    value = sign;
  } else if (exponent == 31) {
    value = sign | 0x7f800000u | (mantissa << 13);
  } else {
    value = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
  }
  float result = 0.f;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

inline float DecodeUnsignedFloat(uint32_t bits, uint32_t mantissa_bits) {
  const uint32_t mantissa_mask = (1u << mantissa_bits) - 1u;
  const uint32_t mantissa = bits & mantissa_mask;
  const uint32_t exponent = (bits >> mantissa_bits) & 0x1fu;
  if (exponent == 0) {
    return std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissa_bits));
  }
  if (exponent == 31) return std::numeric_limits<float>::infinity();
  return std::ldexp(
      1.f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits),
      static_cast<int>(exponent) - 15);
}

// ---------------------------------------------------------------------------
// Color-space conversion. The capture records the game's DLSS color texture in
// its native encoding; hdr_mode (0 = SDR, 1 = linear HDR, 2 = PQ HDR) selects
// the correct decode -> tonemap -> sRGB path so the PNG matches what the
// display shows instead of being washed out (the old code wrote PQ/linear
// values raw, which is the classic F5 wash-out).

// Shoulder knee (in 100-nit units, i.e. relative to SDR reference white):
// pixels below 0.95 pass through untouched; above it a saturating exponential
// rolls highlights towards 1.0.  The old hard threshold at 1.0 kept 100-nit
// pixels at 1.0 but mapped 101-nit pixels to ~0.53 - a ~2x brightness cliff
// that hit essentially every HDR highlight and read as wrong, washed-out
// captures.  This curve is continuous (value AND slope) at the knee, monotone,
// and bounded by 1.0.
constexpr float kHdrShoulderStart = 0.95f;
constexpr float kHdrShoulderTau = 1.5f;

// sRGB OETF: linear scene value in [0,1] -> display-referred [0,1].
inline float EncodeSrgb(float v) {
  v = std::clamp(v, 0.f, 1.f);
  return v <= 0.0031308f ? v * 12.92f
                         : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

// ST 2084 (PQ) EOTF: code in [0,1] -> linear light normalized to 100 nits
// (output of 1.0 == 100 cd/m^2). Used as the tonemap reference white.
inline float PqToLinear100(float code) {
  code = std::clamp(code, 0.f, 1.f);
  if (code <= 0.f) return 0.f;
  const float m1 = 0.1593017578125f;  // 2610 / 16384
  const float m2 = 78.84375f;         // (2523 / 4096) * 128
  const float c1 = 0.8359375f;        // 3424 / 4096
  const float c2 = 18.8515625f;       // 2413 / 128
  const float c3 = 18.6875f;          // 2392 / 128
  const float cp = std::pow(code, 1.f / m2);
  const float num = std::max(cp - c1, 0.f);
  const float den = c2 - c3 * cp;
  const float nits = std::pow(num / den, 1.f / m1);  // 0 .. 10000 nits
  return nits / 100.f;
}

// Smooth HDR shoulder: identity below the knee, saturating exponential above
// (see kHdrShoulderStart).  Linear in, linear out, in 100-nit units.
inline float TonemapHdr(float x) {
  if (x <= kHdrShoulderStart) return x;
  return kHdrShoulderStart
      + (1.f - kHdrShoulderStart)
          * (1.f - std::exp(-(x - kHdrShoulderStart) / kHdrShoulderTau));
}

// Luma-preserving HDR->SDR for an RGB triple (linear, 100-nit normalized).
// SDR-range content is left untouched so midtones stay correct; only HDR
// highlights roll off smoothly towards display white.
inline void TonemapHdrRgb(float& r, float& g, float& b) {
  const float luma_in = std::max(
      0.2126f * r + 0.7152f * g + 0.0722f * b, 0.f);
  if (luma_in <= kHdrShoulderStart) return;
  const float scale = TonemapHdr(luma_in) / luma_in;
  r *= scale;
  g *= scale;
  b *= scale;
}

inline bool ReadbackToRgba8(
    const PendingCapture& capture,
    ID3D12Resource* readback,
    std::vector<uint8_t>& rgba,
    bool* any_nonzero = nullptr) {
  if (readback == nullptr || capture.width == 0 || capture.height == 0
      || capture.row_pitch == 0)
    return false;
  uint32_t bytes_per_pixel = 0;
  switch (capture.format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
      bytes_per_pixel = 4;
      break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      bytes_per_pixel = 8;
      break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
      bytes_per_pixel = 16;
      break;
    default:
      return false;
  }
  D3D12_RANGE range{0, static_cast<SIZE_T>(capture.total_bytes)};
  void* mapped = nullptr;
  if (FAILED(readback->Map(0, &range, &mapped)) || mapped == nullptr) return false;

  // HDR transfer modes only apply to formats that can actually hold HDR
  // signal (float/R11G11B10/R10G10B10A2).  A game that sets the NGX IsHDR
  // flag while writing an ordinary 8-bit UNORM buffer must not be tonemapped
  // and re-sRGB-encoded a second time (its bytes are already display-coded).
  const bool typed_linear =
      capture.format == DXGI_FORMAT_R16G16B16A16_FLOAT
      || capture.format == DXGI_FORMAT_R32G32B32A32_FLOAT
      || capture.format == DXGI_FORMAT_R11G11B10_FLOAT;
  const uint8_t mode =
      (typed_linear || capture.format == DXGI_FORMAT_R10G10B10A2_UNORM)
          ? capture.hdr_mode
          : 0;

  bool saw_nonzero = false;
  rgba.assign(static_cast<size_t>(capture.width) * capture.height * 4u, 0);
  // Rows advance by the full-resource row pitch; the crop base lands once.
  const uint8_t* base = static_cast<const uint8_t*>(mapped)
      + static_cast<size_t>(capture.crop_y) * capture.row_pitch
      + static_cast<size_t>(capture.crop_x) * bytes_per_pixel;
  for (uint32_t y = 0; y < capture.height; ++y) {
    const uint8_t* row = base + static_cast<size_t>(y) * capture.row_pitch;
    for (uint32_t x = 0; x < capture.width; ++x) {
      float r = 0.f, g = 0.f, b = 0.f;
      switch (capture.format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
          r = row[x * 4u] / 255.f; g = row[x * 4u + 1] / 255.f;
          b = row[x * 4u + 2] / 255.f;
          break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
          b = row[x * 4u] / 255.f; g = row[x * 4u + 1] / 255.f;
          r = row[x * 4u + 2] / 255.f;
          break;
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
          const uint32_t v = *reinterpret_cast<const uint32_t*>(row + x * 4u);
          r = (v & 0x3ffu) / 1023.f; g = ((v >> 10) & 0x3ffu) / 1023.f;
          b = ((v >> 20) & 0x3ffu) / 1023.f;
          break;
        }
        case DXGI_FORMAT_R11G11B10_FLOAT: {
          const uint32_t v = *reinterpret_cast<const uint32_t*>(row + x * 4u);
          r = DecodeUnsignedFloat(v & 0x7ffu, 6);
          g = DecodeUnsignedFloat((v >> 11) & 0x7ffu, 6);
          b = DecodeUnsignedFloat((v >> 22) & 0x3ffu, 5);
          break;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
          const auto* q = reinterpret_cast<const uint16_t*>(row + x * 8u);
          r = HalfToFloat(q[0]); g = HalfToFloat(q[1]);
          b = HalfToFloat(q[2]);
          break;
        }
        case DXGI_FORMAT_R32G32B32A32_FLOAT: {
          const auto* q = reinterpret_cast<const float*>(row + x * 16u);
          r = q[0]; g = q[1]; b = q[2];
          break;
        }
        default:
          readback->Unmap(0, nullptr);
          rgba.clear();
          return false;
      }

      if (!std::isfinite(r)) r = 0.f;
      if (!std::isfinite(g)) g = 0.f;
      if (!std::isfinite(b)) b = 0.f;
      r = std::max(0.f, r);
      g = std::max(0.f, g);
      b = std::max(0.f, b);

      // Decode -> tonemap -> sRGB per captured color space.
      if (mode == 2) {
        // PQ-encoded HDR: invert ST 2084, roll off, then sRGB-encode.
        r = PqToLinear100(r);
        g = PqToLinear100(g);
        b = PqToLinear100(b);
        TonemapHdrRgb(r, g, b);
        r = EncodeSrgb(r);
        g = EncodeSrgb(g);
        b = EncodeSrgb(b);
      } else if (mode == 1) {
        // Linear-light HDR float: roll off highlights, then sRGB-encode.
        TonemapHdrRgb(r, g, b);
        r = EncodeSrgb(r);
        g = EncodeSrgb(g);
        b = EncodeSrgb(b);
      } else {
        // SDR: UNORM paths are already display-encoded (passthrough);
        // typed-linear (float) paths need the sRGB OETF.
        if (typed_linear) {
          r = EncodeSrgb(r);
          g = EncodeSrgb(g);
          b = EncodeSrgb(b);
        }
      }

      uint8_t* dst = rgba.data() + (static_cast<size_t>(y) * capture.width + x) * 4u;
      dst[0] = static_cast<uint8_t>(std::lround(std::clamp(r, 0.f, 1.f) * 255.f));
      dst[1] = static_cast<uint8_t>(std::lround(std::clamp(g, 0.f, 1.f) * 255.f));
      dst[2] = static_cast<uint8_t>(std::lround(std::clamp(b, 0.f, 1.f) * 255.f));
      // The PNG alpha is always opaque: HDR intermediates routinely carry an
      // alpha of 0 or NaN, and alpha-respecting viewers composite that to a
      // fully black image - the classic "black screenshot" report.
      dst[3] = 255;
      if (dst[0] != 0 || dst[1] != 0 || dst[2] != 0) saw_nonzero = true;
    }
  }
  readback->Unmap(0, nullptr);
  if (any_nonzero != nullptr) *any_nonzero = saw_nonzero;
  return true;
}

// ---------------------------------------------------------------------------
// Worker thread. Owns everything after the hand-off; must never throw across
// its boundary.

// File work only: rc10's capture, GPU lifetime and ReadbackToRgba8 stay intact.
inline bool WriteCaptureFiles(
    const PendingCapture& work, const std::vector<uint8_t>& pre,
    const std::vector<uint8_t>& post, const std::filesystem::path& base,
    FinishedCapture* record, std::string* meta) {
  renodx::utils::png::internal::ScopedComInitialization com;
  const Microsoft::WRL::ComPtr<IWICImagingFactory> factory =
      com.IsUsable() ? renodx::utils::png::internal::CreateWicFactory() : nullptr;
  const bool want_jpeg = work.output.format != FileFormat::kPng && factory != nullptr;
  const bool want_png = work.output.format != FileFormat::kJpeg || !want_jpeg;
  if (work.output.format != FileFormat::kPng && !want_jpeg) {
    SafeLog(reshade::log::level::warning,
            "NR screenshot: JPEG encoder unavailable; writing PNG instead");
  }
  struct Picture {
    const wchar_t* suffix;
    const std::vector<uint8_t>* rgba;
    uint32_t width;
    bool pair;
    std::vector<uint8_t> bgr;
  };
  std::vector<uint8_t> both;
  std::vector<Picture> pictures{
      {L"_NR_OFF", &pre, work.width, true, {}},
      {L"", &post, work.width, true, {}}};
  if (work.output.side_by_side) {
    both.reserve(pre.size() + post.size());
    for (uint32_t y = 0; y < work.height; ++y) {
      for (const auto* half : {&pre, &post}) {
        const auto row = half->begin() + static_cast<ptrdiff_t>(y) * work.width * 4;
        both.insert(both.end(), row, row + static_cast<ptrdiff_t>(work.width) * 4);
      }
    }
    pictures.push_back({L"_COMPARE", &both, work.width * 2u, false, {}});
  }
  struct File {
    std::filesystem::path path;
    std::vector<uint8_t> bytes;
    bool pair;
  };
  std::vector<File> files;
  bool compressed = true;
  for (Picture& picture : pictures) {
    picture.bgr.resize(picture.rgba->size() / 4u * 3u);
    for (size_t pixel = 0; pixel < picture.rgba->size() / 4u; ++pixel) {
      picture.bgr[pixel * 3u] = (*picture.rgba)[pixel * 4u + 2u];
      picture.bgr[pixel * 3u + 1u] = (*picture.rgba)[pixel * 4u + 1u];
      picture.bgr[pixel * 3u + 2u] = (*picture.rgba)[pixel * 4u];
    }
    if (want_png) {
      std::vector<uint8_t> png;
      if (!WicEncode(factory.Get(), false, picture.width, work.height,
                     picture.bgr.data(), false, 1.f, &png)) {
        png = EncodePngRgba8Stored(picture.width, work.height, *picture.rgba);
        compressed = false;
      }
      files.push_back({base.wstring() + picture.suffix + L".png", std::move(png), picture.pair});
    }
  }
  const uint32_t requested = std::clamp(work.output.jpeg_quality, 80u, 100u);
  const uint32_t lowest = std::min(85u, requested);
  const uint64_t budget = static_cast<uint64_t>(work.output.max_megabytes) * 1000000ull;
  uint32_t pair_quality = 0;
  uint32_t compare_quality = 0;
  bool chroma_444 = false;
  bool over_budget = false;
  if (want_jpeg) {
    for (const bool pair : {true, false}) {
      std::vector<File> encoded;
      for (uint32_t quality = requested;; quality = std::max(lowest, quality - 3u)) {
        encoded.clear();
        bool fits = true;
        for (const Picture& picture : pictures) {
          if (picture.pair != pair) continue;
          std::vector<uint8_t> jpeg;
          if (!WicEncode(factory.Get(), true, picture.width, work.height,
                          picture.bgr.data(), false, quality / 100.f, &jpeg, &chroma_444)) {
            encoded.clear();
            SafeLog(reshade::log::level::error, "NR screenshot JPEG encode failed");
            break;
          }
          fits = fits && (budget == 0 || jpeg.size() <= budget);
          encoded.push_back({base.wstring() + picture.suffix + L".jpg", std::move(jpeg), pair});
        }
        if (encoded.empty() || fits || quality == lowest) {
          (pair ? pair_quality : compare_quality) = encoded.empty() ? 0 : quality;
          over_budget = over_budget || (!encoded.empty() && !fits);
          break;
        }
      }
      if (pair && encoded.empty() && !want_png) return false;
      for (File& file : encoded) files.push_back(std::move(file));
    }
  }
  bool pair_ok = true;
  bool extra_ok = true;
  std::string names;
  std::string sizes;
  for (const File& file : files) {
    bool& ok = file.pair ? pair_ok : extra_ok;
    ok = WriteBytes(file.path, file.bytes) && ok;
    const std::string name = PathToLogString(file.path.filename());
    names += (names.empty() ? "" : ",") + name;
    sizes += (sizes.empty() ? "" : ",") + name + ":" + std::to_string(file.bytes.size());
  }
  if (!pair_ok) {
    for (const File& file : files) {
      std::error_code ignored;
      if (file.pair) std::filesystem::remove(file.path, ignored);
    }
    return false;
  }
  if (!extra_ok) SafeLog(reshade::log::level::warning, "NR screenshot side-by-side write failed");
  const std::wstring primary = want_png ? L".png" : L".jpg";
  record->pre_image = base.wstring() + L"_NR_OFF" + primary;
  record->post_image = base.wstring() + primary;
  if (over_budget) {
    SafeLog(reshade::log::level::warning,
            "NR screenshot JPEG exceeds the size limit at minimum quality; kept at full resolution");
  }
  SafeLog(reshade::log::level::info, "NR screenshot files: " + sizes + ", SDR sRGB");
  *meta += std::string("file_format=")
      + (want_png && want_jpeg ? "png+jpeg" : want_png ? "png" : "jpeg")
      + "\nlayout=" + (work.output.side_by_side ? "pair+side-by-side" : "pair")
      + "\nfiles=" + names + "\nbytes=" + sizes
      + "\npng_bits=8\npng_encoder=" + (!want_png ? "none" : compressed ? "wic-deflate" : "stored")
      + "\nsource=dlss-output\ncolour=srgb-preview\n";
  if (want_jpeg) {
    *meta += "jpeg_quality=" + std::to_string(pair_quality)
        + "\njpeg_compare_quality=" + std::to_string(compare_quality)
        + "\njpeg_requested_quality=" + std::to_string(requested)
        + "\njpeg_budget_bytes=" + std::to_string(budget)
        + "\njpeg_over_budget=" + (over_budget ? "1" : "0")
        + "\njpeg_chroma=" + (chroma_444 ? "444" : "encoder-default") + "\n";
  }
  return true;
}

inline void ProcessJob(std::optional<PendingCapture> work) noexcept {
  if (!work.has_value()) return;
  FinishedCapture record;
  record.serial = work->serial;
  record.outcome = "exception";
  try {
    if (work->aborted || !work->ready) {
      if (work->aborted) {
        SafeLog(reshade::log::level::info, "NR screenshot aborted; no files written");
      } else {
        SafeLog(reshade::log::level::error, "NR screenshot not ready; no files written");
      }
      record.outcome = work->aborted ? "aborted" : "not_ready";
      PublishFinished(std::move(record));
      ReleaseReadbacks(*work);
      return;
    }

    // All filesystem work happens here on the worker, never on the render or
    // present thread. Wide paths only: narrowing to ANSI can throw.
    std::filesystem::path directory;
    try {
      directory =
          renodx::utils::path::GetReShadeBasePath() / std::filesystem::path(L"DLSS5 Screenshots");
      std::error_code ec;
      std::filesystem::create_directories(directory, ec);
    } catch (...) {
      directory.clear();
    }

    std::filesystem::path base = directory.empty()
        ? std::filesystem::path(
              L"NR_CAPTURE_" + std::to_wstring(work->timestamp_ms) + L"_"
              + std::to_wstring(work->serial))
        : directory / std::filesystem::path(
              L"NR_CAPTURE_" + std::to_wstring(work->timestamp_ms) + L"_"
              + std::to_wstring(work->serial));
    std::vector<uint8_t> pre_pixels;
    std::vector<uint8_t> post_pixels;
    bool pre_nonzero = false;
    bool post_nonzero = false;
    const bool read_ok =
        ReadbackToRgba8(*work, work->pre_nr_readback, pre_pixels, &pre_nonzero)
        && ReadbackToRgba8(*work, work->nr_output_readback, post_pixels, &post_nonzero);
    if (read_ok && (!pre_nonzero || !post_nonzero)) {
      // Zero-filled readbacks mean the copies had not executed on the GPU when
      // the buffers were mapped (or the scene really was black).  Say so
      // instead of shipping a mysterious black PNG.
      SafeLog(
          reshade::log::level::warning,
          std::string("NR screenshot pair contains an entirely black image")
              + (!pre_nonzero ? " [pre-NR]" : "")
              + (!post_nonzero ? " [NR on]" : "")
              + "; if the scene was not dark, the GPU copies had not finished"
                " when the readback was mapped - take the screenshot again");
    }
    const bool wrote = read_ok && WriteCaptureFiles(*work, pre_pixels, post_pixels, base, &record, &work->meta);
    record.outcome = wrote ? "written" : "write_failed";
    if (!wrote) {
      SafeLog(reshade::log::level::error, "NR screenshot readback or file write failed");
    }
    // Raw float planes + sidecar meta: the exposure/units evidence the PNGs
    // cannot carry.  Row-major float32 RGBA, no header; dims live in the meta.
    for (const internal::DiagnosticPlane& plane : work->planes) {
      if (plane.readback == nullptr) continue;
      std::filesystem::path plane_path = base;
      plane_path += L"_" + std::wstring(plane.name.begin(), plane.name.end())
          + L".f32";
      std::error_code plane_ec;
      std::filesystem::remove(plane_path, plane_ec);
      std::ofstream out(plane_path, std::ios::binary | std::ios::trunc);
      bool plane_ok = out.good() && plane.width > 0u && plane.height > 0u;
      if (plane_ok) {
        void* mapped = nullptr;
        if (SUCCEEDED(plane.readback->Map(0, nullptr, &mapped))
            && mapped != nullptr) {
          std::vector<float> row(static_cast<size_t>(plane.width) * 4u);
          const auto* base_row = static_cast<const uint8_t*>(mapped);
          for (uint32_t y = 0; y < plane.height && plane_ok; ++y) {
            const auto* src = reinterpret_cast<const uint16_t*>(
                base_row + static_cast<size_t>(y) * plane.footprint.Footprint.RowPitch);
            for (uint32_t x = 0; x < plane.width; ++x) {
              row[x * 4u] = HalfToFloat(src[x * 4u]);
              row[x * 4u + 1] = HalfToFloat(src[x * 4u + 1]);
              row[x * 4u + 2] = HalfToFloat(src[x * 4u + 2]);
              row[x * 4u + 3] = HalfToFloat(src[x * 4u + 3]);
            }
            out.write(
                reinterpret_cast<const char*>(row.data()),
                static_cast<std::streamsize>(row.size() * sizeof(float)));
            plane_ok = out.good();
          }
          plane.readback->Unmap(0, nullptr);
        } else {
          plane_ok = false;
        }
      }
      if (plane_ok) {
        SafeLog(
            reshade::log::level::info,
            "NR diagnostic plane written: " + PathToLogString(plane_path)
                + " (" + plane.name + ", " + std::to_string(plane.width) + "x"
                + std::to_string(plane.height) + " f32)");
      } else {
        SafeLog(
            reshade::log::level::error,
            "NR diagnostic plane write failed: " + PathToLogString(plane_path));
      }
    }
    if (!work->meta.empty()) {
      std::filesystem::path meta_path = base;
      meta_path += L".meta.txt";
      std::ofstream meta_out(meta_path, std::ios::trunc);
      if (meta_out.good()) {
        meta_out << work->meta;
        meta_out.flush();
        if (meta_out.good()) record.meta = meta_path;
        SafeLog(
            reshade::log::level::info,
            "NR capture meta written: " + PathToLogString(meta_path));
      }
    }
  } catch (const std::exception& ex) {
    SafeLog(reshade::log::level::error, std::string("NR screenshot exception: ") + ex.what());
  } catch (...) {
    SafeLog(reshade::log::level::error, "NR screenshot unknown exception");
  }
  PublishFinished(std::move(record));
  ReleaseReadbacks(*work);
}

inline void WorkerLoop() noexcept {
  // Pin the addon module for the lifetime of this thread.  The worker is
  // detached by design (see the header contract), so a mid-session FreeLibrary
  // of the addon - a ReShade addon reload - would otherwise unmap the code
  // this loop is executing.  The pinned reference is deliberately never
  // released; the OS reclaims it at process end.
  HMODULE pinned_module = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
      reinterpret_cast<LPCWSTR>(&WorkerLoop),
      &pinned_module);
  for (;;) {
    std::optional<PendingCapture> work;
    {
      std::unique_lock lock(job_mutex);
      job_signal.wait(lock, [] {
        return shutdown_requested.load(std::memory_order_acquire) || job.has_value();
      });
      if (!job.has_value()) return;  // shutdown requested and queue drained
      work = std::move(*job);
      job.reset();
    }
    ProcessJob(std::move(work));
  }
}

// Hands a settled capture to the worker. Called on the present thread with no
// other lock held; must not throw.
inline void HandoffToWorker(std::optional<PendingCapture> work) noexcept {
  if (!work.has_value()) return;
  if (shutdown_requested.load(std::memory_order_acquire)) {
    ReleaseReadbacks(*work);
    return;
  }
  {
    std::scoped_lock lock(job_mutex);
    if (job.has_value()) {
      // A newer capture supersedes one the worker has not picked up yet.
      ReleaseReadbacks(*job);
      job.reset();
      SafeLog(reshade::log::level::info, "NR screenshot superseded before write");
    }
    job = std::move(work);
  }
  if (!worker_started.exchange(true, std::memory_order_acq_rel)) {
    bool created = false;
    try {
      std::thread(WorkerLoop).detach();
      created = true;
    } catch (...) {
    }
    if (!created) {
      // No worker available: degrade to synchronous completion rather than
      // leaving the feature wedged. One present-thread stall, never a crash.
      worker_started.store(false, std::memory_order_release);
      std::optional<PendingCapture> fallback;
      {
        std::scoped_lock lock(job_mutex);
        fallback = std::move(job);
        job.reset();
      }
      ProcessJob(std::move(fallback));
      return;
    }
  }
  job_signal.notify_one();
}

}  // namespace internal

// ---------------------------------------------------------------------------
// Public API. Every entry point is noexcept-safe; callers on the render path
// (ProcessInline) hold dlssnr's runtime_mutex, and state_mutex is always
// nested inside it, never the other way around.

inline bool IsArmed() {
  return internal::capture_requested.load(std::memory_order_acquire);
}

inline bool HasPending() {
  return internal::has_pending.load(std::memory_order_acquire);
}

inline void Disarm() {
  internal::capture_requested.store(false, std::memory_order_release);
}

// Arms the one-shot capture. Ignored while a capture is already armed or
// waiting for its GPU completion window.
inline bool RequestCapture() {
  std::scoped_lock lock(internal::state_mutex);
  bool expected = false;
  if (!internal::capture_requested.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return false;
  }
  const uint32_t format = file_format.load(std::memory_order_relaxed);
  internal::requested_output = {
      format <= 2u ? static_cast<FileFormat>(format) : FileFormat::kPng,
      layout.load(std::memory_order_relaxed) == 1u,
      jpeg_quality.load(std::memory_order_relaxed),
      max_megabytes.load(std::memory_order_relaxed)};
  internal::presents_while_armed.store(0, std::memory_order_relaxed);
  internal::SafeLog(
      reshade::log::level::info, "NR screenshot armed for the next successful evaluation");
  return true;
}

// Edge-detected capture-hotkey poll, called once per present with the
// configurable virtual-key code (owned by dlssnr.hpp) and whether the game
// has the foreground: GetAsyncKeyState is system-wide, so without it the key
// pressed in another application captured the game.  Only a single user32
// call plus two atomic accesses when idle.
inline void PollHotkey(uint32_t virtual_key, bool focused) {
  const bool down = focused
      && (GetAsyncKeyState(static_cast<int>(virtual_key)) & 0x8000) != 0;
  if (!down) {
    internal::hotkey_was_down.store(false, std::memory_order_relaxed);
    return;
  }
  if (internal::hotkey_was_down.exchange(true, std::memory_order_relaxed)) return;
  RequestCapture();
}

// Area (crop dimensions) of the currently pending capture, 0 when none.  The
// evaluate-side gate uses it to keep a larger main-pass capture from being
// superseded by a smaller secondary pass in the same frame.
inline uint64_t PendingCaptureArea() {
  if (!internal::has_pending.load(std::memory_order_acquire)) return 0;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending.has_value()) return 0;
  return static_cast<uint64_t>(internal::pending->width)
      * internal::pending->height;
}

// Allocates the two addon-owned READBACK buffers for one evaluation pair.
// Runs on the render thread with runtime_mutex held; performs no filesystem
// work and can therefore not throw.  width/height are the CAPTURE (crop)
// dimensions; crop_x/crop_y select a subrect when the engine renders into
// part of a larger DLSS output resource (clamped against the resource here).
inline bool PrepareCapture(
    ID3D12Device* device,
    ID3D12Resource* source,
    uint32_t crop_x,
    uint32_t crop_y,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    uint8_t hdr_mode,
    uint64_t generation) {
  if (device == nullptr || source == nullptr || width == 0 || height == 0)
    return false;
  const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 total_bytes = 0;
  device->GetCopyableFootprints(
      &source_desc, 0, 1, 0, &footprint, &rows, nullptr, &total_bytes);
  if (total_bytes == 0 || footprint.Footprint.RowPitch == 0) return false;
  if (crop_x >= footprint.Footprint.Width
      || crop_y >= footprint.Footprint.Height) {
    return false;
  }
  if (crop_x + width > footprint.Footprint.Width) {
    width = footprint.Footprint.Width - crop_x;
  }
  if (crop_y + height > footprint.Footprint.Height) {
    height = footprint.Footprint.Height - crop_y;
  }
  if (width == 0 || height == 0) return false;

  internal::PendingCapture capture;
  capture.width = width;
  capture.height = height;
  capture.crop_x = crop_x;
  capture.crop_y = crop_y;
  capture.footprint = footprint;
  capture.total_bytes = total_bytes;
  capture.row_pitch = footprint.Footprint.RowPitch;
  capture.format = format;
  capture.hdr_mode = hdr_mode;
  capture.timestamp_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  capture.serial = internal::capture_serial.fetch_add(1) + 1;
  capture.creation_generation = generation;

  if (!internal::MakeReadbackBuffer(device, total_bytes, &capture.pre_nr_readback)
      || !internal::MakeReadbackBuffer(device, total_bytes, &capture.nr_output_readback)) {
    internal::ReleaseReadbacks(capture);
    return false;
  }
  std::scoped_lock lock(internal::state_mutex);
  capture.output = internal::requested_output;
  if (internal::pending.has_value()) {
    // A prior evaluate in this same capture-frame already recorded a pair into
    // its own buffers. Retire that ENTIRE buffer set - the PNG pair and every
    // diagnostic plane (still GPU-pending on the frame's command list) - so it
    // is released at the same settle window as the final capture; the latest
    // evaluate's pair wins.
    internal::RetiredCaptureBuffers retired;
    retired.pre_nr_readback = internal::pending->pre_nr_readback;
    retired.nr_output_readback = internal::pending->nr_output_readback;
    retired.planes = std::move(internal::pending->planes);
    internal::retired_buffers.push_back(std::move(retired));
  }
  internal::pending = std::move(capture);
  internal::has_pending.store(true, std::memory_order_release);
  return true;
}

// Records the pre-NR copy: pre_nr_source must hold the untouched DLSS output
// and be in D3D12_RESOURCE_STATE_COPY_SOURCE when called.
inline void RecordPreCopy(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* pre_nr_source) {
  if (command_list == nullptr || pre_nr_source == nullptr) return;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending.has_value()) return;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = internal::pending->pre_nr_readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = internal::pending->footprint;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = pre_nr_source;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

// Records the post-NR copy and completes the pair: nr_source must hold the
// final decoded result and be in D3D12_RESOURCE_STATE_COPY_SOURCE.
inline void RecordPostCopy(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* nr_source) {
  if (command_list == nullptr || nr_source == nullptr) return;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending.has_value()) return;
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = internal::pending->nr_output_readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = internal::pending->footprint;
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = nr_source;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  internal::pending->ready = true;
}

// The float planes are a developer's evidence, ini-only (NRScreenshotPlanes,
// set by dlssnr.hpp's LoadConfiguration; off by default since v7.0.0-rc10).
// An HDR capture records four or more of them, each ~130 MB on disk at 4K
// plus its readback heap, which a player's screenshot key should not cost.
inline std::atomic_bool diagnostic_planes_enabled = false;

// Allocates the readback buffer for one named float plane (e.g. the sRGB
// proxy, the resolved work surface).  Call while a capture is pending, before
// the chain records its copies; no-op when no capture is in flight or the
// planes are off, and every plane's copy then records nothing.
inline bool PrepareDiagnosticPlane(
    ID3D12Device* device,
    const char* name,
    ID3D12Resource* texture,
    uint32_t width,
    uint32_t height) {
  if (!diagnostic_planes_enabled.load(std::memory_order_relaxed)) return false;
  if (device == nullptr || name == nullptr || texture == nullptr) return false;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending.has_value()) return false;
  for (const internal::DiagnosticPlane& plane : internal::pending->planes) {
    if (plane.name == name) return true;  // already prepared this frame
  }
  const D3D12_RESOURCE_DESC desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total_bytes = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr,
                                &total_bytes);
  if (total_bytes == 0 || footprint.Footprint.RowPitch == 0) return false;
  internal::DiagnosticPlane plane;
  plane.name = name;
  plane.width = width;
  plane.height = height;
  plane.footprint = footprint;
  plane.total_bytes = total_bytes;
  if (!internal::MakeReadbackBuffer(device, total_bytes, &plane.readback)) return false;
  internal::pending->planes.push_back(std::move(plane));
  return true;
}

// Records one float-plane copy: texture must be in COPY_SOURCE for the call.
inline void RecordDiagnosticPlaneCopy(
    ID3D12GraphicsCommandList* command_list,
    const char* name,
    ID3D12Resource* texture) {
  if (command_list == nullptr || name == nullptr || texture == nullptr) return;
  std::scoped_lock lock(internal::state_mutex);
  if (!internal::pending.has_value()) return;
  for (internal::DiagnosticPlane& plane : internal::pending->planes) {
    if (plane.name != name || plane.readback == nullptr) continue;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = plane.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = plane.footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    return;
  }
}

// Per-stack-pass diagnostic copy: under a multi-pass stack each pass records
// its own plane ("proxy_p2"), exposing what each stacked pass sees and
// returns; a single-pass stack keeps the canonical name ("proxy").  Prepares
// the plane on first use (name-deduped), so stack-1 captures are unchanged.
inline void RecordStackPlaneCopy(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* texture,
    uint32_t width,
    uint32_t height,
    const char* base_name,
    uint32_t pass,
    uint32_t stack) {
  char name[16];
  if (stack > 1) {
    std::snprintf(name, sizeof(name), "%.6s_p%u", base_name, pass);
  } else {
    std::snprintf(name, sizeof(name), "%s", base_name);
  }
  if (!PrepareDiagnosticPlane(device, name, texture, width, height)) return;
  RecordDiagnosticPlaneCopy(command_list, name, texture);
}

// Attaches the sidecar metadata (divisor, encoding, meter state) that the
// .f32 planes need to be interpretable.  No-op when no capture is pending.
inline void SetPendingDiagnosticMeta(std::string meta) {
  std::scoped_lock lock(internal::state_mutex);
  if (internal::pending.has_value()) {
    internal::pending->meta = std::move(meta);
  }
}

// Marks an armed or in-flight capture as dead (device loss, NR evaluate
// failure, new device). The buffers are released by the worker after the
// settle window.
inline void AbortPending() {
  std::scoped_lock lock(internal::state_mutex);
  if (internal::pending.has_value()) internal::pending->aborted = true;
  internal::capture_requested.store(false, std::memory_order_release);
}

// Called once per present with the new generation. Fast path when idle is a
// single atomic load. Settled captures are moved to the worker; all heavy work
// happens off this thread.
inline void OnPresent(uint64_t generation) noexcept {
  // Armed-timeout: an arm that never sees a DLSS evaluation (loading screen,
  // NR off, no DLSS in this scene) would otherwise fire the capture on the
  // first evaluate much later - typically a black frame.  Disarm it after
  // ~10 s of presents and say so.
  if (internal::capture_requested.load(std::memory_order_acquire)
      && !internal::has_pending.load(std::memory_order_acquire)) {
    if (internal::presents_while_armed.fetch_add(1, std::memory_order_relaxed) + 1
        > internal::kArmTimeoutPresents) {
      internal::capture_requested.store(false, std::memory_order_release);
      internal::presents_while_armed.store(0, std::memory_order_relaxed);
      internal::SafeLog(
          reshade::log::level::info,
          "NR screenshot disarmed: no DLSS evaluation ran for about 10 seconds"
          " (F5 needs an active NR evaluation during gameplay)");
    }
  } else {
    internal::presents_while_armed.store(0, std::memory_order_relaxed);
  }
  if (!internal::has_pending.load(std::memory_order_acquire)) return;
  std::optional<internal::PendingCapture> work;
  {
    std::scoped_lock lock(internal::state_mutex);
    if (!internal::pending.has_value()) {
      internal::has_pending.store(false, std::memory_order_release);
      return;
    }
    // The capture-frame's present has occurred: no further evaluates for this
    // frame will run, so the pair already in `pending` is the LAST evaluate's
    // and is the one we keep. Stop arming further frames. (OnPresent bumps the
    // generation before it reaches here, so the capture-frame's present arrives
    // with generation > creation_generation.) If the frame never completed a
    // pair (the last evaluate failed), drop the capture.
    if (generation > internal::pending->creation_generation
        && internal::capture_requested.load(std::memory_order_acquire)) {
      internal::capture_requested.store(false, std::memory_order_release);
      if (!internal::pending->ready && !internal::pending->aborted) {
        internal::pending->aborted = true;
      }
    }
    // The frame presented, so the submissions carrying this frame's copies
    // are queued: one lease over the tracked fences now proves the copies
    // executed once completed.  Covers aborted captures too - their buffers
    // may hold a recorded pre-copy.
    if (generation > internal::pending->creation_generation
        && !internal::pending->lease_taken) {
      internal::pending->lease_taken = true;
      internal::pending->lease_generation = generation;
      internal::pending->lease = AcquireGpuLease();
    }
    // A settle TIMEOUT cancels delivery and reports failure - it never
    // establishes GPU completion, so the buffers are not mapped and not
    // released: they move to the retired set (released behind the next
    // proven settle or at teardown).  A frame count cannot prove a fence.
    const bool empty_lease_timeout =
        internal::pending->lease.empty()
        && generation
               >= internal::pending->creation_generation
                      + internal::kSettlePresents;
    const bool stalled_lease_timeout =
        !internal::pending->lease.empty()
        && !GpuLeaseCompleted(internal::pending->lease)
        && generation
               >= internal::pending->lease_generation
                      + kCaptureLeaseFallbackTicks;
    if (empty_lease_timeout || stalled_lease_timeout) {
      static std::atomic_bool logged_cancel_timeout{false};
      if (!logged_cancel_timeout.exchange(true)) {
        internal::SafeLog(
            reshade::log::level::warning,
            internal::pending->lease.empty()
                ? "NR screenshot cancelled: no tracked queue fence proved the"
                  " copies executed before the timeout (buffers retained, no"
                  " files written)"
                : "NR screenshot cancelled: the tracked queue fence did not"
                  " advance before the timeout (present-starved session?"
                  " buffers retained, no files written)");
      }
      internal::RetiredCaptureBuffers retained;
      retained.Take(std::move(*internal::pending));
      internal::pending.reset();
      internal::has_pending.store(false, std::memory_order_release);
      internal::retired_buffers.push_back(std::move(retained));
      return;
    }
    if (internal::pending->lease.empty()) return;  // still waiting for fences
    if (!GpuLeaseCompleted(internal::pending->lease)) return;
    if (!internal::pending->ready && !internal::pending->aborted) return;
    work = std::move(*internal::pending);
    internal::pending.reset();
    internal::has_pending.store(false, std::memory_order_release);
    // Retired buffers from earlier evaluates in the same frame share the frame's
    // command list and settle together with the final capture.
    for (internal::RetiredCaptureBuffers& retired : internal::retired_buffers) {
      retired.Release();
    }
    internal::retired_buffers.clear();
  }
  internal::HandoffToWorker(std::move(work));
}

// Flag-only shutdown signal: no locks, no state release, no worker join.
// Safe from DLL_PROCESS_DETACH where blocking on a worker-owned mutex could
// stall the loader lock on an in-flight job.  The worker exits after its
// current job; any queued job's readbacks are reclaimed by the OS at process
// end (device rebuilds use AbortPending instead, which does drain state).
inline void SignalShutdown() noexcept {
  internal::capture_requested.store(false, std::memory_order_release);
  internal::shutdown_requested.store(true, std::memory_order_release);
  internal::job_signal.notify_all();
}

}  // namespace renodx::addons::dlss5::screenshot
