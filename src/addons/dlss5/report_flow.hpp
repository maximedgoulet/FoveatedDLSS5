/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The issue report (rc11): "I'm having issues..." in the panel.
//
// Included by dlssnr.hpp INSIDE namespace internal, just before
// the layouts, so it sees the support report, verdict and rc10 screenshot.
// It is not a standalone header.
//
// What the player sees, in the panel:
//   1. a one-line description and six ticks (what went wrong);
//   2. an F5 capture taken while the game keeps running, with a progress bar
//      and a timeout - a report never waits forever on a capture: it bundles
//      what exists and states why the capture is missing;
//   3. a review of every file with its size and a checkbox, and the consent
//      statement (which host, that anyone with the link can download it, when
//      it is deleted); [Upload & copy link] [Save zip only] [Cancel];
//   4. the link, already on the clipboard, and where to paste it - or, when
//      the upload failed, the saved zip's folder, opened.
// Nothing leaves the PC without the Upload click.
//
// Threads: the panel only reads and writes the flow's state under
// issue_report::mutex, a leaf lock never taken with runtime_mutex or
// screenshot's state_mutex.  Capture waits, file IO, the zip and the upload
// run on a worker thread that holds a reference on this module and ends
// through FreeLibraryAndExitThread (OpenLogFolderMain's pattern), so an
// addon unload cannot pull the code out from under it.
//
// Every step states one line: NR-REPORT v1 begin|capture|review|attempt|
// outcome ..., one grammar for support to grep.  No line ever carries the
// link's path: that is the secret (report::LinkForLog).
//
// The panel's strings are in the i18n regions (tools/i18n/ui_strings.py)
// and drawn through ui::Tr since v8.5.0-rc10; through rc9 they were English
// in every language.  Slugs and zip entry names on those lines are tagged
// "i18n: skip", and CaptureNote's result codes are comparisons.
//
// RENODX_NR_TEST_REPORT=<reason> (test only) clicks through the flow by
// itself: begin at present 60, continue, and upload from the review step.
// RENODX_NR_TEST_REPORT_URL replaces every host with a loopback URL (any
// other host is refused), and RENODX_NR_TEST_REPORT_TIMEOUT_MS shortens the
// capture timeout.  Unset, which is every shipped session, none of this is
// registered.

#pragma comment(lib, "advapi32.lib")

namespace issue_report {

enum class Step : uint8_t {
  kClosed = 0,
  kDescribe,    // step 1, in the panel
  kCapturing,   // worker: the F5 capture
  kCollecting,  // worker: logs, redaction, report.json
  kReview,      // step 3, in the panel
  kPacking,     // worker: the zip
  kUploading,   // worker: the host
  kDone,        // step 4, in the panel
};

inline constexpr int64_t kCaptureTimeoutMs = 20'000;
// Text files longer than this retain their startup and their newest events.
inline constexpr size_t kTextTailBytes = 16u << 20;
inline constexpr size_t kKeptZips = 3;

struct Tick {
  const char* slug;
  const char* label;
};
// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)
inline constexpr Tick kTicks[] = {
    {"flicker",  // i18n: skip
     "Flicker or boiling"},
    {"colours",  // i18n: skip
     "Wrong colours"},
    {"crash",  // i18n: skip
     "Crash or freeze"},
    {"fps",  // i18n: skip
     "Low fps"},
    {"nr_not_working",  // i18n: skip
     "NR not working"},
    {"other",  // i18n: skip
     "Something else"},
};
// i18n: end

struct Item {
  const char* label;
  std::string entry;           // the name inside the zip
  std::filesystem::path file;  // staged (redacted) copy, or the capture's own PNG
  uint64_t bytes = 0;
  bool include = true;
  bool required = false;
};

struct ActivitySnapshot {
  uint64_t presents = 0;
  uint64_t ngx_creates = 0;
  uint64_t ngx_evaluations = 0;
  uint64_t eligible_evaluations = 0;
  uint64_t streamline_evaluations = 0;
  uint64_t streamline_dlss_evaluations = 0;
  uint64_t streamline_tags = 0;
  uint64_t nr_after_evaluations = 0;
  uint64_t nr_before_evaluations = 0;
  bool before_selected = false;
};

inline std::mutex mutex;
// Under `mutex`.
inline std::string reason = "button";
inline std::string description;
inline uint32_t ticks = 0;  // one bit per kTicks entry
inline std::string capture_result;
inline std::string status_at_start;
inline std::string dx11_at_start;
inline ActivitySnapshot activity_at_start;
inline std::vector<Item> items;
inline std::string summary;  // report.json, open: the send job closes it
inline std::filesystem::path stage;
inline std::filesystem::path zip;
inline std::string link;  // the secret: the panel and the clipboard only
inline std::string error;
inline std::string host_display;
inline std::string host_setting = report::kDefaultHosts;  // NRReportHost

inline std::atomic<Step> step{Step::kClosed};
inline std::atomic_bool cancel{false};
inline std::atomic_bool skip_capture{false};
inline std::atomic_bool upload{false};
inline std::atomic_int64_t capture_started_ns{0};
inline report::UploadProgress progress;

// The panel's own copy while the player types; present thread only.
inline char typed[256] = {};
inline uint32_t typed_ticks = 0;

// Test only; set at attach, then read-only.
inline std::string test_reason;
inline std::wstring test_url;
inline int64_t test_timeout_ms = 0;
inline uint32_t test_stall_from = 0;

}  // namespace issue_report

// This is an observation test over the report's existing capture window. It
// touches no game resources and works even when no NR frame is available.
inline issue_report::ActivitySnapshot ReportActivitySnapshot() {
  return {
      presents_observed.load(std::memory_order_relaxed),
      intercepted_creates.load(std::memory_order_relaxed),
      intercepted_evaluations.load(std::memory_order_relaxed),
      ComputeNrVerdict(CollectFunnelInputs()).eligible,
      intercepted_streamline_evaluations.load(std::memory_order_relaxed),
      intercepted_streamline_dlss_evaluations.load(std::memory_order_relaxed),
      intercepted_streamline_tag_calls.load(std::memory_order_relaxed),
      successful_evaluations.load(std::memory_order_relaxed),
      successful_pre_sr_evaluations.load(std::memory_order_relaxed),
      nr_before_upscale.load(std::memory_order_relaxed)};
}

// A live snapshot, independent of the periodic telemetry line. The report
// takes it twice because a 20-second capture wait can outlive the symptom.
inline std::string Dx11ReportSnapshot() {
  const uint32_t source = dx11_source.load(std::memory_order_relaxed);
  const uint8_t route = dx11_route.load(std::memory_order_acquire);
  const BridgeMotionDiagnosticSnapshot motion = ReadBridgeMotionDiagnostic();
  char motion_hr[16], tier_hr[16];
  std::snprintf(motion_hr, sizeof(motion_hr), "0x%08X", motion.hresult);
  std::snprintf(tier_hr, sizeof(tier_hr), "0x%08X", motion.shared_resource_tier_hresult);
  return "{\"session\": " + std::string(D3D11OnlySession() ? "true" : "false")
      + ", \"source\": " + report::JsonString(
            source == kDx11SourceNative ? "native"
            : source == kDx11SourceForeign ? "foreign"
            : source == kDx11SourceOff ? "off" : "auto")
      + ", \"route\": " + report::JsonString(
            route == kDx11RouteNative ? "native"
            : route == kDx11RouteForeign ? "foreign" : "undecided")
      + ", \"foreign_tool_seen\": "
      + (foreign_dx11_tool_seen.load(std::memory_order_relaxed) ? "true" : "false")
      + ", \"module_scans\": " + std::to_string(module_scans.load(std::memory_order_relaxed))
      + ", \"bridge_state\": "
      + report::JsonString(BridgeStateName(bridge_state.load(std::memory_order_acquire)))
      + ", \"device_removed\": "
      + (bridge_device_removed.load(std::memory_order_relaxed) ? "true" : "false")
      + ", \"signals\": " + std::to_string(bridge_signals.load(std::memory_order_relaxed))
      + ", \"waits\": " + std::to_string(bridge_waits.load(std::memory_order_relaxed))
      + ", \"submits\": " + std::to_string(bridge_submits.load(std::memory_order_relaxed))
      + ", \"waits11\": " + std::to_string(bridge_waits11.load(std::memory_order_relaxed))
      + ", \"timeouts\": " + std::to_string(bridge_timeouts.load(std::memory_order_relaxed))
       + ", \"skew\": " + std::to_string(bridge_skew.load(std::memory_order_relaxed))
       + ", \"motion_transport\": {\"source_format\": "
       + std::to_string(motion.source_format)
       + ", \"consistent\": " + (motion.consistent ? "true" : "false")
       + ", \"selected_mode\": " + report::JsonString(BridgeMotionModeName(motion.selected_mode))
       + ", \"selected_format\": " + std::to_string(motion.selected_transport_format)
       + ", \"selected_bind_flags\": " + std::to_string(motion.selected_bind_flags)
       + ", \"failed_format\": " + std::to_string(motion.last_failed_transport_format)
       + ", \"failed_bind_flags\": " + std::to_string(motion.last_failed_bind_flags)
       + ", \"failed_step\": " + report::JsonString(BridgeMotionStepName(motion.failed_step))
       + ", \"hresult\": " + report::JsonString(motion_hr)
       + ", \"failure_count\": " + std::to_string(motion.failure_count)
       + ", \"shared_resource_tier\": " + std::to_string(motion.shared_resource_tier)
       + ", \"shared_resource_tier_hresult\": " + report::JsonString(tier_hr)
       + "}}";
}

// Opens the report's first step, as if the player had pressed "I'm having
// issues...".  reason_slug names what asked: "button", a health ledger row's
// slug, "test".  Safe from any thread (the flow's leaf lock only); false
// while a report is already under way, which the panel then shows.
inline bool BeginIssueReport(const char* reason_slug) {
  using issue_report::Step;
  Step expected = Step::kClosed;
  if (!issue_report::step.compare_exchange_strong(expected, Step::kDescribe)) {
    expected = Step::kDone;
    if (!issue_report::step.compare_exchange_strong(expected, Step::kDescribe)) return false;
  }
  issue_report::cancel = false;
  issue_report::skip_capture = false;
  issue_report::upload = false;
  std::string reason = reason_slug != nullptr && reason_slug[0] != '\0' ? reason_slug : "button";
  {
    std::lock_guard lock(issue_report::mutex);
    issue_report::reason = reason;
    issue_report::items.clear();
    issue_report::capture_result.clear();
    issue_report::link.clear();
    issue_report::error.clear();
    issue_report::zip.clear();
  }
  Log(reshade::log::level::info, "NR-REPORT v1 begin reason=" + reason);
  return true;
}

// Shows the zip in Explorer (selected), from a thread that is not the
// game's: ShellExecute loads shell extensions.  Never in a test session.
inline void ShowReportZip() {
  std::filesystem::path zip;
  {
    std::lock_guard lock(issue_report::mutex);
    zip = issue_report::zip;
  }
  if (zip.empty() || !issue_report::test_reason.empty()) return;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const std::wstring select = L"/select,\"" + zip.wstring() + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
  if (SUCCEEDED(com)) CoUninitialize();
}

inline DWORD WINAPI ShowReportZipMain(void* module) {
  ShowReportZip();
  FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

inline std::string FormatBytes(uint64_t bytes) {
  char text[32];
  if (bytes >= (1ull << 20)) {
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1 << 20));
  } else {
    std::snprintf(text, sizeof(text), "%.0f KB",
                  std::ceil(static_cast<double>(bytes) / 1024.0));
  }
  return text;
}

// The worker's first job: the F5 capture, then everything the review lists.
inline void RunReportCaptureJob() {
  using issue_report::Step;
  const int64_t started_ns = SteadyNowNs();
  issue_report::capture_started_ns = started_ns;
  const int64_t timeout_ms = issue_report::test_timeout_ms > 0 ? issue_report::test_timeout_ms
                                                                : issue_report::kCaptureTimeoutMs;
  uint64_t armed_at = 0;
  bool armed = false;
  while (!armed && !issue_report::cancel && !issue_report::skip_capture
         && SteadyNowNs() - started_ns < timeout_ms * 1'000'000) {
    if (!screenshot::IsArmed() && !screenshot::HasPending()) {
      armed_at = (std::max)(static_cast<uint64_t>(screenshot::internal::capture_serial.load()),
                            screenshot::internal::finished_serial.load());
      armed = screenshot::RequestCapture();
    }
    if (!armed) Sleep(50);
  }
  screenshot::internal::FinishedCapture capture;
  std::string result;
  if (!armed) {
    result = issue_report::cancel ? "cancelled"
             : issue_report::skip_capture ? "skipped" : "capture_busy";
  }
  while (result.empty()) {
    const bool prepared = screenshot::internal::capture_serial.load() != armed_at;
    if (screenshot::internal::finished_serial.load(std::memory_order_acquire) > armed_at) {
      std::lock_guard lock(screenshot::internal::finished_mutex);
      capture = screenshot::internal::finished;
      result = capture.outcome;
    } else if (issue_report::cancel || issue_report::skip_capture) {
      if (!prepared) screenshot::Disarm();
      result = issue_report::cancel ? "cancelled" : "skipped";
    } else if (!prepared && !screenshot::IsArmed()) {
      result = "no_nr_frame";  // the arm ran out: no NR evaluation for ~10 s
    } else if (SteadyNowNs() - started_ns >= timeout_ms * 1'000'000) {
      if (!prepared) screenshot::Disarm();
      result = prepared ? "timeout" : "no_nr_frame";
    } else {
      Sleep(50);
    }
  }
  const issue_report::ActivitySnapshot activity_after = ReportActivitySnapshot();
  const bool sl_common_loaded = GetModuleHandleW(L"sl.common.dll") != nullptr;
  const bool sl_interposer_loaded = GetModuleHandleW(L"sl.interposer.dll") != nullptr;
  const bool sl_dlss_loaded = GetModuleHandleW(L"sl.dlss.dll") != nullptr;
  const bool ngx_core_loaded = GetModuleHandleW(L"_nvngx.dll") != nullptr
                               || GetModuleHandleW(L"nvngx.dll") != nullptr;
  const bool ngx_dlss_loaded = GetModuleHandleW(L"nvngx_dlss.dll") != nullptr;
  const bool ngx11_hooked = ngx11_any_hooked.load(std::memory_order_relaxed);
  const bool ngx12_hooked = ngx_any_hooked.load(std::memory_order_relaxed);
  const bool nr_enabled = enabled.load(std::memory_order_relaxed);
  const bool hooks_active = hooks_enabled.load(std::memory_order_relaxed);
  const bool streamline_hook_mode = streamline_hooks_enabled.load(std::memory_order_relaxed);
  const int64_t capture_ms = (SteadyNowNs() - started_ns) / 1'000'000;
  Log(reshade::log::level::info,
      "NR-REPORT v1 capture result=" + result + " ms=" + std::to_string(capture_ms)
          + " serial=" + std::to_string(capture.serial));
  if (issue_report::cancel) {
    issue_report::step = Step::kClosed;
    Log(reshade::log::level::info, "NR-REPORT v1 outcome=cancelled step=capture");
    return;
  }
  issue_report::step = Step::kCollecting;

  // Everything below is file IO into <ReShade folder>\DLSS5 Reports.
  const report::Redaction redaction = report::SystemRedaction();
  std::filesystem::path base = renodx::utils::path::GetReShadeBasePath();
  if (base.empty()) base = AddonDirectory();
  SYSTEMTIME now{};
  GetLocalTime(&now);
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth,
                now.wDay, now.wHour, now.wMinute, now.wSecond);
  const std::filesystem::path stage = base / L"DLSS5 Reports" / (std::string("report-") + stamp);
  std::error_code ignored;
  std::filesystem::create_directories(stage, ignored);

  std::vector<issue_report::Item> items;
  const auto read_text = [](const std::filesystem::path& path) {
    return report::ReadTextExcerpt(path, issue_report::kTextTailBytes).text;
  };
  const auto add_text = [&](const char* label, const std::string& entry, std::string_view text) {
    if (text.empty()) return;
    const std::string redacted = report::RedactText(text, redaction);
    const std::filesystem::path file = stage / std::filesystem::path(entry).filename();
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(redacted.data(), static_cast<std::streamsize>(redacted.size()));
    if (out.good()) items.push_back({label, entry, file, redacted.size()});
  };
  const std::string support = BuildSupportReport();
  std::string status_at_start;
  std::string dx11_at_start;
  issue_report::ActivitySnapshot activity_at_start;
  {
    std::lock_guard lock(issue_report::mutex);
    status_at_start = issue_report::status_at_start;
    dx11_at_start = issue_report::dx11_at_start;
    activity_at_start = issue_report::activity_at_start;
  }
  // i18n: begin - the review step's file labels, drawn through ui::Tr
  add_text("Support report",
           "support-report.txt", support);  // i18n: skip
  add_text("Support report",
           "support-report-at-start.txt", status_at_start);  // i18n: skip
  // i18n: end
  // ReShade writes ReShade.log beside ReShade.ini, or ReShade.log1..9 when
  // another process holds it. Choose the newest and report evidence of a
  // game/version match; modification time alone cannot prove ownership.
  std::filesystem::path reshade_log;
  std::filesystem::file_time_type newest{};
  for (int suffix = 0; suffix <= 9; ++suffix) {
    const std::filesystem::path candidate =
        base / (suffix == 0 ? std::string("ReShade.log") : "ReShade.log" + std::to_string(suffix));
    const auto written = std::filesystem::last_write_time(candidate, ignored);
    if (!ignored && (reshade_log.empty() || written > newest)) {
      reshade_log = candidate;
      newest = written;
    }
    ignored.clear();
  }
  const report::TextExcerpt reshade_excerpt = reshade_log.empty()
      ? report::TextExcerpt{}
      : report::ReadTextExcerpt(reshade_log, issue_report::kTextTailBytes);
  const std::string& log_text = reshade_excerpt.text;
  bool log_time_checked = false;
  bool log_written_since_start = false;
  if (!reshade_log.empty()) {
    const HANDLE file = CreateFileW(reshade_log.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      FILETIME written{}, created{}, exited{}, kernel{}, user{};
      log_time_checked = GetFileTime(file, nullptr, nullptr, &written) != FALSE
                         && GetProcessTimes(GetCurrentProcess(), &created, &exited,
                                            &kernel, &user) != FALSE;
      if (log_time_checked) log_written_since_start = CompareFileTime(&written, &created) >= 0;
      CloseHandle(file);
    }
  }
  const bool log_game_seen = log_text.find(GameExeName()) != std::string::npos;
  const bool log_addon_seen = log_text.find(kAddonVersion) != std::string::npos;
  // i18n: begin - the review step's file labels, drawn through ui::Tr
  add_text("ReShade log",
           "logs/ReShade.log", log_text);  // i18n: skip
  add_text("ReShade settings (ReShade.ini)",
           "logs/ReShade.ini", read_text(base / "ReShade.ini"));  // i18n: skip
  add_text("Crash log",
           "logs/RenoDX-DLSS5-crash.log",  // i18n: skip
           lastgasp::crash_log_path.empty() ? std::string()
               : read_text(lastgasp::crash_log_path));
  if (norm_trace_enabled.load(std::memory_order_relaxed)) {
    add_text("Normalization trace",
             "logs/RenoDX-DLSS5-normtrace.csv",  // i18n: skip
             read_text(AddonDirectory() / "RenoDX-DLSS5-normtrace.csv"));  // i18n: skip
  }
  if (edit_trace_enabled.load(std::memory_order_relaxed)) {
    add_text("Edit trace",
             "logs/RenoDX-DLSS5-edittrace.csv",  // i18n: skip
             read_text(AddonDirectory() / "RenoDX-DLSS5-edittrace.csv"));  // i18n: skip
  }
#if defined(RENODX_DLSS5_DEBUG)
  add_text("Debug trace",
           "logs/RenoDX-DLSS5-debug.log",  // i18n: skip
           debug::trace_path.empty() ? std::string()
               : read_text(debug::trace_path));
#endif
  // The capture's PNG when it wrote one, else its JPEG; the entry keeps the
  // extension (ours: .png or .jpg, ASCII).
  const auto add_image = [&](const char* label, const char* entry, const std::filesystem::path& file) {
    const uint64_t bytes = file.empty() ? 0 : std::filesystem::file_size(file, ignored);
    if (!file.empty() && !ignored) {
      items.push_back({label, entry + file.extension().string(), file, bytes});
    }
    ignored.clear();
  };
  add_image(capture.model_diagnostic ? "Model output diagnostic" : "Screenshot, NR on",
            capture.model_diagnostic ? "capture/MODEL_OUTPUT" : "capture/NR_ON",  // i18n: skip
            capture.post_image);
  add_image(capture.model_diagnostic ? "Model input diagnostic" : "Screenshot, NR off",
            capture.model_diagnostic ? "capture/MODEL_INPUT" : "capture/NR_OFF",  // i18n: skip
            capture.pre_image);
  if (!capture.meta.empty()) {
    add_text("Capture details",
             "capture/capture.meta.txt", read_text(capture.meta));  // i18n: skip
  }
  // i18n: end

  // report.json: what support needs first, as data.  Closed by the send
  // job, which appends what the player left out.
  const NrVerdict verdict = ComputeNrVerdict(CollectFunnelInputs());
  const ui::StatusCard card = ui::ComputeStatusCard(CollectCardInputs(verdict, SteadyNowNs()));
  std::string json = "{\n  \"schema\": \"renodx-dlss5-report/1\"";
  const auto field = [&json](const char* key, const std::string& value) {
    json += ",\n  " + report::JsonString(key) + ": " + value;
  };
  std::string ticks = "[";
  std::string description;
  {
    std::lock_guard lock(issue_report::mutex);
    field("reason", report::JsonString(issue_report::reason));
    description = issue_report::description;
    for (size_t i = 0; i < std::size(issue_report::kTicks); ++i) {
      if ((issue_report::ticks >> i) & 1u) {
        ticks += (ticks.size() > 1 ? ", " : "") + report::JsonString(issue_report::kTicks[i].slug);
      }
    }
  }
  field("description", report::JsonString(report::RedactText(description, redaction)));
  field("ticks", ticks + "]");
  wchar_t addon_path[MAX_PATH * 4] = {};
  GetModuleFileNameW(addon_module, addon_path, static_cast<DWORD>(std::size(addon_path)));
  field("addon", "{\"version\": " + report::JsonString(kAddonVersion) + ", \"build\": "
                     + report::JsonString(__DATE__ " " __TIME__) + ", \"md5\": "
                     + report::JsonString(report::FileDigestHex(addon_path, BCRYPT_MD5_ALGORITHM))
                     + "}");
  field("game", "{\"exe\": " + report::JsonString(GameExeName()) + ", \"api\": "
                    + report::JsonString(verdict.api) + ", \"path\": "
                    + report::JsonString(verdict.path) + "}");
  field("verdict", "{\"state\": " + report::JsonString(NrStateName(verdict.state))
                       + ", \"stage\": " + report::JsonString(verdict.StageName())
                       + ", \"reason\": " + report::JsonString(verdict.ReasonName())
                       + ", \"injected\": " + std::to_string(verdict.injected)
                       + ", \"eligible\": " + std::to_string(verdict.eligible) + "}");
  field("card", "{\"id\": " + report::JsonString(ui::CardSlug(card.id)) + ", \"tone\": "
                    + report::JsonString(ui::CardToneName(card.tone)) + "}");
  // The support report's settings/counters lines as objects, its telemetry
  // line as text.
  for (const char* key : {"settings", "counters", "telemetry"}) {
    const std::string prefix = std::string(key) + ": ";
    const size_t at = support.find("\n" + prefix);
    if (at == std::string::npos) continue;
    const size_t begin = at + 1 + prefix.size();
    const std::string line = support.substr(begin, support.find('\n', begin) - begin);
    if (std::strcmp(key, "telemetry") == 0) {
      field(key, report::JsonString(line));
      continue;
    }
    std::string object = "{";
    for (size_t token = 0; token < line.size();) {
      const size_t end = (std::min)(line.find(' ', token), line.size());
      const std::string pair = line.substr(token, end - token);
      const size_t equals = pair.find('=');
      if (equals != std::string::npos) {
        object += (object.size() > 1 ? ", " : "") + report::JsonString(pair.substr(0, equals))
                  + ": " + report::JsonString(pair.substr(equals + 1));
      }
      token = end + 1;
    }
    field(key, object + "}");
  }
  // The machine: first hardware adapter, its driver, the OS build, ReShade.
  std::string gpu;
  std::string driver;
  if (renodx::utils::directx::Initialize() && renodx::utils::directx::pCreateDXGIFactory1 != nullptr) {
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(renodx::utils::directx::pCreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      IDXGIAdapter1* adapter = nullptr;
      for (UINT index = 0; gpu.empty() && factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND;
           ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        LARGE_INTEGER umd{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
          gpu = report::Utf8(desc.Description);
          if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            char text[64];
            const auto high = static_cast<uint32_t>(umd.HighPart);
            const auto low = static_cast<uint32_t>(umd.LowPart);
            // NVIDIA's public number is the last five digits: 32.0.15.8129 is 581.29.
            if (desc.VendorId == 0x10DE) {
              const uint32_t nvidia = (HIWORD(low) % 10) * 10000 + LOWORD(low);
              std::snprintf(text, sizeof(text), "%u.%02u (%u.%u.%u.%u)", nvidia / 100, nvidia % 100,
                            HIWORD(high), LOWORD(high), HIWORD(low), LOWORD(low));
            } else {
              std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(high), LOWORD(high),
                            HIWORD(low), LOWORD(low));
            }
            driver = text;
          }
        }
        adapter->Release();
      }
      factory->Release();
    }
  }
  std::string os;
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  if (const auto get_version = reinterpret_cast<RtlGetVersionFn>(
          GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
    OSVERSIONINFOW version{sizeof(version)};
    DWORD revision = 0;
    DWORD revision_bytes = sizeof(revision);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                 RRF_RT_REG_DWORD, nullptr, &revision, &revision_bytes);
    if (get_version(&version) == 0) {
      os = std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion)
           + "." + std::to_string(version.dwBuildNumber) + "." + std::to_string(revision);
    }
  }
  std::string reshade_version;
  if (const size_t at = log_text.find("ReShade version '"); at != std::string::npos) {
    const size_t begin = at + std::strlen("ReShade version '");
    reshade_version = log_text.substr(begin, log_text.find('\'', begin) - begin);
  }
  field("system", "{\"gpu\": " + report::JsonString(gpu) + ", \"driver\": "
                      + report::JsonString(driver) + ", \"os\": " + report::JsonString(os)
                      + ", \"reshade\": " + report::JsonString(reshade_version) + "}");
  field("nr_runtime", "{\"sha256\": " + report::JsonString(direct_runtime_sha256)
                          + ", \"reference_match\": "
                          + (direct_runtime_reference_match ? "true" : "false")
                          + ", \"file_version\": "
                          + report::JsonString(NgxLoaderView().nr_file_version) + "}");
  field("capture", "{\"result\": " + report::JsonString(result) + ", \"ms\": "
                       + std::to_string(capture_ms) + ", \"armed\": "
                       + (armed ? "true" : "false") + ", \"comparison\": "
                       + report::JsonString(result != "written" ? "none"
                             : capture.model_diagnostic ? "model-pass-input-output"
                                                        : "dlss-output-same-evaluate-sdr-preview")
                         + "}");
  const auto delta = [](uint64_t before, uint64_t after) {
    return after >= before ? after - before : 0;
  };
  const uint64_t new_presents = delta(activity_at_start.presents, activity_after.presents);
  const uint64_t new_ngx_creates = delta(activity_at_start.ngx_creates, activity_after.ngx_creates);
  const uint64_t new_ngx_evaluations =
      delta(activity_at_start.ngx_evaluations, activity_after.ngx_evaluations);
  const uint64_t new_eligible = delta(activity_at_start.eligible_evaluations,
                                      activity_after.eligible_evaluations);
  const uint64_t new_sl_evaluations =
      delta(activity_at_start.streamline_evaluations, activity_after.streamline_evaluations);
  const uint64_t new_sl_dlss = delta(activity_at_start.streamline_dlss_evaluations,
                                     activity_after.streamline_dlss_evaluations);
  const uint64_t new_sl_tags =
      delta(activity_at_start.streamline_tags, activity_after.streamline_tags);
  const uint64_t new_nr_after = delta(activity_at_start.nr_after_evaluations,
                                      activity_after.nr_after_evaluations);
  const uint64_t new_nr_before = delta(activity_at_start.nr_before_evaluations,
                                       activity_after.nr_before_evaluations);
  const uint64_t new_nr = new_nr_after + new_nr_before;
  const char* before_result =
      activity_at_start.before_selected != activity_after.before_selected
          ? "selection_changed_during_capture"
      : !activity_after.before_selected ? "not_selected"
      : new_nr_before > 0 ? "engaged_before_upscaling"
      : !nr_enabled ? "nr_disabled"
      : !hooks_active ? "hooks_disabled"
      : new_nr_after > 0 && logged_pre_sr_rr.load(std::memory_order_relaxed)
          ? "after_path_with_ray_reconstruction_exception_seen"
      : new_eligible == 0 ? "no_eligible_evaluate_observed"
      : "selected_without_before_upscaling_evaluate";
  const char* activity_result = new_nr > 0 ? "nr_engaged"
      : new_presents == 0 ? "no_present_observed"
      : !nr_enabled ? "nr_disabled"
      : !hooks_active ? "hooks_disabled"
      : capture_ms < 1000 ? "short_observation"
      : new_eligible > 0 ? "eligible_evaluate_without_nr"
      : new_sl_dlss > 0 ? "streamline_dlss_without_nr"
      : new_ngx_evaluations > 0 ? "ngx_evaluate_seen_no_eligible_nr"
      : new_ngx_creates > 0 ? "ngx_create_without_evaluate"
      : "no_dlss_evaluate_observed";
  const char* log_result = reshade_log.empty() ? "missing"
      : log_text.empty() ? "unreadable_or_empty"
      : log_time_checked && !log_written_since_start ? "older_than_process"
      : !log_time_checked ? "time_unavailable"
      : !log_game_seen || !log_addon_seen ? "identity_unconfirmed"
      : "likely_current";
  Log(reshade::log::level::info,
      "NR-REPORT v1 self-test activity=" + std::string(activity_result)
          + " presents=" + std::to_string(new_presents)
          + " ngx_evaluations=" + std::to_string(new_ngx_evaluations)
          + " eligible=" + std::to_string(new_eligible)
          + " streamline_dlss_evaluations=" + std::to_string(new_sl_dlss)
          + " nr_evaluations=" + std::to_string(new_nr)
          + " nr_before=" + std::to_string(new_nr_before)
          + " before_selected=" + std::to_string(activity_after.before_selected)
          + " before_result=" + before_result
          + " log=" + log_result);
  const bool crash_log_included = std::any_of(items.begin(), items.end(),
      [](const issue_report::Item& item) {
        return item.entry == "logs/RenoDX-DLSS5-crash.log";
      });
  std::ostringstream self_tests;
  self_tests << std::boolalpha
             << "{\"activity\": {\"result\": " << report::JsonString(activity_result)
             << ", \"window_ms\": " << capture_ms
             << ", \"presents\": " << new_presents
             << ", \"ngx_creates\": " << new_ngx_creates
             << ", \"ngx_evaluations\": " << new_ngx_evaluations
             << ", \"eligible_evaluations\": " << new_eligible
             << ", \"streamline_evaluations\": " << new_sl_evaluations
             << ", \"streamline_dlss_evaluations\": " << new_sl_dlss
             << ", \"streamline_tag_calls\": " << new_sl_tags
             << ", \"nr_evaluations\": " << new_nr
             << ", \"nr_before_upscaling_evaluations\": " << new_nr_before
             << ", \"nr_after_upscaling_evaluations\": " << new_nr_after
             << ", \"before_upscaling_result\": " << report::JsonString(before_result)
             << ", \"before_upscaling_selected_at_start\": "
             << activity_at_start.before_selected
             << ", \"before_upscaling_selected_after_capture\": "
             << activity_after.before_selected
             << ", \"ray_reconstruction_after_exception_seen\": "
             << logged_pre_sr_rr.load(std::memory_order_relaxed)
             << "}, \"route\": {\"nr_enabled\": " << nr_enabled
             << ", \"hooks_enabled\": " << hooks_active
             << ", \"ngx11_hooked\": " << ngx11_hooked
             << ", \"ngx12_hooked\": " << ngx12_hooked
             << ", \"streamline_hook_mode\": " << streamline_hook_mode
             << ", \"sl_common_loaded\": " << sl_common_loaded
             << ", \"sl_interposer_loaded\": " << sl_interposer_loaded
             << ", \"sl_dlss_loaded\": " << sl_dlss_loaded
             << ", \"ngx_core_loaded\": " << ngx_core_loaded
             << ", \"ngx_dlss_loaded\": " << ngx_dlss_loaded
             << "}, \"evidence\": {\"capture\": " << report::JsonString(result)
             << ", \"reshade_log\": " << report::JsonString(log_result)
             << ", \"crash_dump_requested\": " << lastgasp::dumps_on.load()
             << ", \"crash_log_included\": " << crash_log_included
             << ", \"minidump_writer_ready\": "
             << (lastgasp::write_minidump.load(std::memory_order_acquire) != nullptr)
             << "}}";
  field("self_tests", self_tests.str());
  field("dx11", "{\"at_start\": " + dx11_at_start
                  + ", \"after_capture\": " + Dx11ReportSnapshot() + "}");
  field("diagnostic_options", "{\"crash_dump\": "
                                  + std::string(lastgasp::dumps_on.load() ? "true" : "false")
                                  + ", \"gpu_timers\": "
                                  + (gpu_timers_enabled.load() ? "true" : "false")
                                  + ", \"norm_trace\": "
                                  + (norm_trace_enabled.load() ? "true" : "false")
                                  + ", \"edit_trace\": "
                                  + (edit_trace_enabled.load() ? "true" : "false")
                                  + ", \"screenshot_planes\": "
                                  + (screenshot::diagnostic_planes_enabled.load() ? "true" : "false")
                                  + "}");
  field("log_collection", "{\"reshade_file\": "
                              + report::JsonString(reshade_log.empty() ? ""
                                    : reshade_log.filename().string())
                              + ", \"reshade_bytes\": "
                              + std::to_string(reshade_excerpt.file_bytes)
                              + ", \"reshade_omitted_bytes\": "
                              + std::to_string(reshade_excerpt.omitted_bytes)
                               + ", \"game_seen\": "
                               + (log_game_seen ? "true" : "false")
                               + ", \"addon_version_seen\": "
                               + (log_addon_seen ? "true" : "false")
                              + "}");
  std::string available = "[";
  for (const issue_report::Item& item : items) {
    available += (available.size() > 1 ? ", " : "") + report::JsonString(item.entry);
  }
  field("available_files", available + "]");

  uint64_t total = json.size();
  for (const issue_report::Item& item : items) total += item.bytes;
  // i18n: begin - the review step's file labels, drawn through ui::Tr
  items.insert(items.begin(),
               issue_report::Item{"Summary: version, status, settings, GPU and driver",
                                  "report.json", {}, json.size(), true, true});  // i18n: skip
  // i18n: end
  const size_t files = items.size();
  std::string hosts = issue_report::test_url.empty() ? std::string() : "test";
  {
    std::lock_guard lock(issue_report::mutex);
    std::string unknown;
    for (const report::Host* host : report::ParseHosts(issue_report::host_setting, &unknown)) {
      if (issue_report::test_url.empty()) hosts += (hosts.empty() ? "" : ",") + std::string(host->id);
    }
    issue_report::capture_result = result;
    issue_report::items = std::move(items);
    issue_report::summary = std::move(json);
    issue_report::stage = stage;
  }
  Log(reshade::log::level::info,
      "NR-REPORT v1 review files=" + std::to_string(files) + " bytes="
          + std::to_string(total) + " capture=" + result
          + " hosts=" + (hosts.empty() ? "none" : hosts));
  issue_report::step = Step::kReview;
}

// The worker's second job: the zip, then the upload when the player asked
// for one.
inline void RunReportSendJob() {
  using issue_report::Step;
  const int64_t started_ns = SteadyNowNs();
  std::vector<issue_report::Item> items;
  std::string json;
  std::filesystem::path stage;
  std::string host_setting;
  {
    std::lock_guard lock(issue_report::mutex);
    items = issue_report::items;
    json = issue_report::summary;
    stage = issue_report::stage;
    host_setting = issue_report::host_setting;
  }
  std::string left_out = "[";
  for (const issue_report::Item& item : items) {
    if (!item.include) left_out += (left_out.size() > 1 ? ", " : "") + report::JsonString(item.entry);
  }
  json += ",\n  \"left_out\": " + left_out + "]\n}\n";
  const std::filesystem::path reports = stage.parent_path();
  const std::filesystem::path zip =
      reports / ("renodx-dlss5-" + stage.filename().string() + ".zip");
  report::ZipWriter writer(zip);
  writer.AddBytes("report.json", json.data(), json.size());
  for (const issue_report::Item& item : items) {
    if (item.include && !item.required) writer.AddFile(item.entry, item.file);
  }
  const bool packed = writer.Finish();
  const uint64_t zip_bytes = writer.Bytes();
  std::error_code ignored;
  std::filesystem::remove_all(stage, ignored);
  // The newest kKeptZips reports stay; their names sort by time.
  std::vector<std::filesystem::path> zips;
  for (const auto& entry : std::filesystem::directory_iterator(reports, ignored)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("renodx-dlss5-report-", 0) == 0 && entry.path().extension() == ".zip") {
      zips.push_back(entry.path());
    }
  }
  std::sort(zips.begin(), zips.end());
  for (size_t i = 0; i + issue_report::kKeptZips < zips.size(); ++i) {
    std::filesystem::remove(zips[i], ignored);
  }
  {
    std::lock_guard lock(issue_report::mutex);
    issue_report::zip = packed ? zip : std::filesystem::path();
  }
  const std::string zip_name = zip.filename().string();
  const auto finish = [&](const char* outcome, const std::string& detail) {
    Log(std::strcmp(outcome, "uploaded") == 0 || std::strcmp(outcome, "saved") == 0
            ? reshade::log::level::info
            : reshade::log::level::warning,
        std::string("NR-REPORT v1 outcome=") + outcome + " bytes=" + std::to_string(zip_bytes)
            + " ms=" + std::to_string((SteadyNowNs() - started_ns) / 1'000'000) + " zip="
            + (packed ? zip_name : "-") + detail);
    issue_report::step = Step::kDone;
  };
  if (!packed) {
    {
      std::lock_guard lock(issue_report::mutex);
      issue_report::error = "the zip could not be written in " + report::Utf8(reports.wstring());
    }
    finish("pack_failed", "");
    return;
  }
  // A test posts litterbox's form to its loopback server.
  std::vector<const report::Host*> hosts;
  if (issue_report::upload) {
    hosts = issue_report::test_url.empty() ? report::ParseHosts(host_setting, nullptr)
                                           : std::vector<const report::Host*>{&report::kHosts[0]};
  }
  if (hosts.empty()) {
    ShowReportZip();
    finish("saved", issue_report::upload ? " hosts=none" : "");
    return;
  }
  issue_report::step = Step::kUploading;
  const std::wstring agent = L"RenoDX-DLSS5/" + std::wstring(kAddonVersion, kAddonVersion + std::strlen(kAddonVersion));
  report::UploadResult result;
  const report::Host* answered = nullptr;
  for (const report::Host* host : hosts) {
    const char* id = issue_report::test_url.empty() ? host->id : "test";
    {
      std::lock_guard lock(issue_report::mutex);
      issue_report::host_display = issue_report::test_url.empty() ? host->display : "loopback test server";
    }
    Log(reshade::log::level::info,
        std::string("NR-REPORT v1 attempt host=") + id + " bytes=" + std::to_string(zip_bytes));
    result = report::Upload(*host, issue_report::test_url.empty() ? host->url : issue_report::test_url,
                            zip, agent, &issue_report::progress);
    if (result.ok || result.cancelled) {
      answered = host;
      break;
    }
    Log(reshade::log::level::warning,
        std::string("NR-REPORT v1 attempt host=") + id + " failed: " + result.error);
  }
  if (result.cancelled) {
    finish("cancelled", "");
    return;
  }
  if (!result.ok) {
    {
      std::lock_guard lock(issue_report::mutex);
      issue_report::error = result.error;
    }
    ShowReportZip();
    finish("upload_failed", " error=\"" + result.error + "\"");
    return;
  }
  // The link to the clipboard now, so it is there whether or not the
  // overlay is open when the upload ends.  A test session leaves the
  // developer's clipboard alone.
  bool copied = false;
  if (issue_report::test_url.empty() && OpenClipboard(nullptr)) {
    const std::wstring wide(result.link.begin(), result.link.end());  // IsLink: ASCII only
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t))) {
      std::memcpy(GlobalLock(memory), wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
      GlobalUnlock(memory);
      EmptyClipboard();
      copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
      if (!copied) GlobalFree(memory);
    }
    CloseClipboard();
  }
  {
    std::lock_guard lock(issue_report::mutex);
    issue_report::link = result.link;
  }
  finish("uploaded", std::string(" host=")
                         + (issue_report::test_url.empty() ? answered->id : "test")
                         + " link=" + report::LinkForLog(result.link)
                         + " clipboard=" + (copied ? "copied" : issue_report::test_url.empty() ? "failed" : "skipped_test"));
}

inline DWORD WINAPI IssueReportMain(void* module) {
  try {
    if (issue_report::step.load() == issue_report::Step::kCapturing) {
      RunReportCaptureJob();
    } else {
      RunReportSendJob();
    }
  } catch (const std::exception& exception) {
    {
      std::lock_guard lock(issue_report::mutex);
      issue_report::error = exception.what();
    }
    Log(reshade::log::level::warning,
        std::string("NR-REPORT v1 outcome=failed error=\"") + exception.what() + "\"");
    issue_report::step = issue_report::Step::kDone;
  }
  FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
}

// Runs `main` on a thread of its own that holds a reference on this module.
inline bool StartReportThread(LPTHREAD_START_ROUTINE main) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(main),
                          &self)) {
    return false;
  }
  if (HANDLE thread = CreateThread(nullptr, 0, main, self, 0, nullptr)) {
    CloseHandle(thread);
    return true;
  }
  FreeLibrary(self);
  return false;
}

// Step 1 -> 2: keeps what the player typed and starts the capture.
inline void ContinueIssueReport() {
  const std::string status = BuildSupportReport();
  const std::string dx11 = Dx11ReportSnapshot();
  const issue_report::ActivitySnapshot activity = ReportActivitySnapshot();
  {
    std::lock_guard lock(issue_report::mutex);
    issue_report::description = issue_report::typed;
    issue_report::ticks = issue_report::typed_ticks;
    issue_report::status_at_start = status;
    issue_report::dx11_at_start = dx11;
    issue_report::activity_at_start = activity;
  }
  issue_report::step = issue_report::Step::kCapturing;
  if (!StartReportThread(IssueReportMain)) {
    Log(reshade::log::level::warning, "NR-REPORT v1 outcome=failed error=\"no worker thread\"");
    issue_report::step = issue_report::Step::kClosed;
  }
}

// Step 3 -> 4: the zip, and the upload when `with_upload`.
inline void SendIssueReport(bool with_upload) {
  issue_report::upload = with_upload;
  issue_report::progress.cancel = false;
  issue_report::step = issue_report::Step::kPacking;
  if (!StartReportThread(IssueReportMain)) {
    Log(reshade::log::level::warning, "NR-REPORT v1 outcome=failed error=\"no worker thread\"");
    issue_report::step = issue_report::Step::kReview;
  }
}

// Cancel in step 1 or 3, Close in step 4: back to the button.
inline void CloseIssueReport() {
  const issue_report::Step at = issue_report::step.load();
  if (at == issue_report::Step::kReview) {
    std::filesystem::path stage;
    {
      std::lock_guard lock(issue_report::mutex);
      stage = issue_report::stage;
    }
    std::error_code ignored;
    if (!stage.empty()) std::filesystem::remove_all(stage, ignored);
    Log(reshade::log::level::info, "NR-REPORT v1 outcome=cancelled step=review");
  } else if (at == issue_report::Step::kDescribe) {
    Log(reshade::log::level::info, "NR-REPORT v1 outcome=cancelled step=describe");
  }
  issue_report::typed[0] = '\0';
  issue_report::typed_ticks = 0;
  issue_report::step = issue_report::Step::kClosed;
}

// What the panel says about a capture result (English; drawn through Tr).
// i18n: begin - display strings (tools/i18n/ui_strings.py catalog)
inline const char* CaptureNote(const std::string& result) {
  if (result == "written") return "Capture taken.";
  if (result == "unsupported_present") return "No capture: this API or presentation path does not support an exact final-frame comparison yet.";
  if (result == "unsupported_diagnostic") return "No capture: model-buffer readback is unavailable on this path; use the game's screenshot function for the displayed image.";
  if (result == "unproven_baseline") return "No capture: exact comparison unavailable with this frame-generation route; disable frame generation for the capture.";
  if (result == "nr_disabled") return "No capture: enable NR before taking a comparison.";
  if (result == "foreign_nr") return "No capture: another NR implementation owns this frame.";
  if (result == "unexecuted") return "No capture: the command list carrying its copies did not execute.";
  if (result == "no_nr_frame") {
    return "No capture: NR did not run a frame (NR off, a menu or a loading screen).";
  }
  if (result == "timeout") return "No capture: it did not finish in time.";
  if (result == "gpu_unconfirmed") return "No capture: the GPU did not confirm the copies in time.";
  if (result == "aborted" || result == "not_ready") {
    return "No capture: the frame's NR pass did not complete.";
  }
  if (result == "skipped") return "No capture: skipped.";
  if (result == "capture_busy") return "No capture: another capture stayed busy.";
  return "No capture: the files could not be written.";
}

// The report, drawn under the status card: the button while closed, the
// current step otherwise. The capture is
// named by the key that takes it (NRScreenshotKey), not a fixed F5.
inline void DrawIssueReport() {
  using issue_report::Step;
  const Step at = issue_report::step.load();
  const std::string capture_key = VirtualKeyName(screenshot_hotkey.load());
  if (at == Step::kClosed) {
    if (ImGui::Button(ui::Tr("I'm having issues..."))) {
      BeginIssueReport("button");  // i18n: skip
    }
    ui::ItemTooltip(
        ui::Tr("Takes a capture (%s) and collects the logs into one zip. You review every"
               " file; it is uploaded only when you press Upload, and you get a link to send"
               " to the mod author."),
        capture_key.c_str());
    return;
  }

  ui::StyleScope frame;
  frame.Color(ImGuiCol_ChildBg, ui::tokens::kBgCard)
      .Var(ImGuiStyleVar_WindowPadding, ImVec2(ui::tokens::kCardPadding, ui::tokens::kCardPadding));
  if (ImGui::BeginChild("##nr_issue_report", ImVec2(0.f, 0.f),
                        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding)) {
    ImGui::PushTextWrapPos(0.f);
    ui::CardHeading({.tone = ui::CardTone::kIdle, .glyph = ui::CardGlyph::kInfo,
                     .title = "Report an issue"});
    const auto progress_bar = [](float fraction) {
      ImGui::ProgressBar(std::clamp(fraction, 0.f, 1.f), ImVec2(-FLT_MIN, 0.f), "");
    };
    switch (at) {
      case Step::kDescribe: {
        ImGui::TextUnformatted(ui::Tr("Step 1 of 3: what went wrong?"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##issue_description",
                                 ui::Tr("One line: what you saw, and where in the game (optional)"),
                                 issue_report::typed, sizeof(issue_report::typed));
        for (size_t i = 0; i < std::size(issue_report::kTicks); ++i) {
          bool ticked = ((issue_report::typed_ticks >> i) & 1u) != 0;
          ImGui::PushID(static_cast<int>(i));
          const char* label = ui::Tr(issue_report::kTicks[i].label);
          if (i % 2 == 1) ui::SameLineIfFits(ImGui::CalcTextSize(label).x
                                             + ImGui::GetFrameHeight() * 2.f);
          if (ImGui::Checkbox(label, &ticked)) {
            issue_report::typed_ticks ^= 1u << i;
          }
          ImGui::PopID();
        }
        ImGui::TextDisabled(
            ui::Tr("Next, the addon takes a capture (%s) while the game keeps running and"
                   " collects its logs. Nothing leaves your PC until you press Upload."),
            capture_key.c_str());
        if (ImGui::Button(ui::Tr("Continue"))) ContinueIssueReport();
        ImGui::SameLine();
        if (ImGui::Button(ui::Tr("Cancel"))) CloseIssueReport();
        break;
      }
      case Step::kCapturing: {
        ImGui::TextUnformatted(ui::Tr("Step 2 of 3: capturing"));
        ImGui::TextDisabled(
            ui::Tr("Taking a capture (%s). Keep playing: a menu or loading screen has no NR"
                   " frame to capture."),
            capture_key.c_str());
        const int64_t timeout_ms = issue_report::test_timeout_ms > 0
                                       ? issue_report::test_timeout_ms
                                       : issue_report::kCaptureTimeoutMs;
        progress_bar(static_cast<float>(SteadyNowNs() - issue_report::capture_started_ns.load())
                     / (static_cast<float>(timeout_ms) * 1e6f));
        if (ImGui::Button(ui::Tr("Continue without a capture"))) issue_report::skip_capture = true;
        ImGui::SameLine();
        if (ImGui::Button(ui::Tr("Cancel"))) issue_report::cancel = true;
        break;
      }
      case Step::kCollecting:
        ImGui::TextUnformatted(ui::Tr("Collecting the logs..."));
        break;
      case Step::kReview: {
        ImGui::TextUnformatted(ui::Tr("Step 3 of 3: review what is sent"));
        bool send = false;
        bool save = false;
        bool close = false;
        {
        std::lock_guard lock(issue_report::mutex);
        ImGui::TextDisabled("%s", ui::Tr(CaptureNote(issue_report::capture_result)));
        uint64_t total = 0;
        for (size_t i = 0; i < issue_report::items.size(); ++i) {
          issue_report::Item& item = issue_report::items[i];
          ImGui::PushID(static_cast<int>(i));
          ImGui::BeginDisabled(item.required);
          ImGui::Checkbox(ui::Tr(item.label), &item.include);
          ImGui::EndDisabled();
          ImGui::PopID();
          ImGui::SameLine();
          ImGui::TextDisabled("%s, %s", item.entry.c_str(), FormatBytes(item.bytes).c_str());
          if (item.include) total += item.bytes;
        }
        ImGui::Text(ui::Tr("Total: %s"), FormatBytes(total).c_str());
        const std::vector<const report::Host*> hosts =
            report::ParseHosts(issue_report::host_setting, nullptr);
        const bool can_upload = !hosts.empty() || !issue_report::test_url.empty();
        if (!issue_report::test_url.empty()) {
          ImGui::TextUnformatted("Upload sends the zip to the loopback test server.");  // i18n: skip (test only)
        } else if (can_upload) {
          ImGui::Text(
              ui::Tr("Upload sends the zip to %s. Anyone with the link can download it; it is"
                     " deleted after %u hours."),
              hosts.front()->display, hosts.front()->retention_hours);
          if (hosts.size() > 1) {
            ImGui::TextDisabled(ui::Tr("If it does not answer: %s, deleted after %u hours."),
                                hosts[1]->display, hosts[1]->retention_hours);
          }
        } else {
          ImGui::TextUnformatted(
              ui::Tr("Uploading is off (NRReportHost=none): the zip is saved on this PC."));
        }
        ImGui::TextDisabled("%s", ui::Tr("Your Windows user name and profile folder are removed"
                                         " from every text file."));
        if (can_upload) {
          send = ImGui::Button(ui::Tr("Upload & copy link"));
          ImGui::SameLine();
        }
        save = ImGui::Button(ui::Tr("Save zip only"));
        ImGui::SameLine();
        close = ImGui::Button(ui::Tr("Cancel"));
        }
        // Out of the lock: both take it.
        if (send || save) SendIssueReport(send);
        if (close) CloseIssueReport();
        break;
      }
      case Step::kPacking:
        ImGui::TextUnformatted(ui::Tr("Packing the zip..."));
        break;
      case Step::kUploading: {
        std::string host;
        {
          std::lock_guard lock(issue_report::mutex);
          host = issue_report::host_display;
        }
        ImGui::Text(ui::Tr("Uploading to %s..."), host.c_str());
        const uint64_t total = issue_report::progress.total.load();
        progress_bar(total == 0 ? 0.f
                                : static_cast<float>(issue_report::progress.sent.load())
                                      / static_cast<float>(total));
        if (ImGui::Button(ui::Tr("Cancel"))) issue_report::progress.cancel = true;
        break;
      }
      case Step::kDone: {
        std::string link;
        std::string error;
        std::filesystem::path zip;
        {
          std::lock_guard lock(issue_report::mutex);
          link = issue_report::link;
          error = issue_report::error;
          zip = issue_report::zip;
        }
        if (!link.empty()) {
          ImGui::TextColored(ui::tokens::kGood, "%s %s", ui::icons::kCheck,
                             ui::Tr("Link copied to the clipboard"));
          ImGui::SetNextItemWidth(-FLT_MIN);
          ImGui::InputText("##issue_link", link.data(), link.size() + 1, ImGuiInputTextFlags_ReadOnly);
          ImGui::TextUnformatted(ui::Tr("Paste this link to the mod author on Discord."));
          if (ImGui::Button(ui::Tr("Copy again"))) ImGui::SetClipboardText(link.c_str());
          ImGui::SameLine();
        } else {
          if (!error.empty()) {
            ImGui::TextColored(ui::tokens::kWarn, ui::Tr("The upload failed: %s"), error.c_str());
          }
          if (!zip.empty()) {
            ImGui::Text(ui::Tr("Saved: %s"), report::Utf8(zip.filename().wstring()).c_str());
            ImGui::TextUnformatted(ui::Tr(
                "Send this file to the mod author on Discord; its folder opens with the button."));
            if (ImGui::Button(ui::Tr("Open folder"))) StartReportThread(ShowReportZipMain);
            ImGui::SameLine();
          }
        }
        if (ImGui::Button(ui::Tr("Close"))) CloseIssueReport();
        break;
      }
      case Step::kClosed:
        break;
    }
    ImGui::PopTextWrapPos();
  }
  ImGui::EndChild();
}
// i18n: end

// ---------------------------------------------------------------------------
// The test driver (RENODX_NR_TEST_REPORT*)
// ---------------------------------------------------------------------------

inline bool InitIssueReportTest() {
  char buffer[512] = {};
  size_t length = 0;
  if (getenv_s(&length, buffer, sizeof(buffer), "RENODX_NR_TEST_REPORT") != 0 || length <= 1) {
    return false;
  }
  issue_report::test_reason = buffer;
  length = 0;
  if (getenv_s(&length, buffer, sizeof(buffer), "RENODX_NR_TEST_REPORT_URL") == 0 && length > 1) {
    // Loopback only: a test never reaches a real host.
    const std::string url = buffer;
    if (url.rfind("http://127.0.0.1:", 0) == 0 || url.rfind("http://localhost:", 0) == 0) {
      issue_report::test_url.assign(url.begin(), url.end());
    } else {
      Log(reshade::log::level::warning,
          "RENODX_NR_TEST_REPORT_URL is not a loopback URL; the test report will not upload");
      issue_report::test_url = L"http://127.0.0.1:1/refused";
    }
  } else {
    issue_report::test_url = L"http://127.0.0.1:1/unset";
  }
  length = 0;
  if (getenv_s(&length, buffer, sizeof(buffer), "RENODX_NR_TEST_REPORT_TIMEOUT_MS") == 0
      && length > 1) {
    issue_report::test_timeout_ms = std::strtoll(buffer, nullptr, 10);
  }
  // Presents are held (not the capture's: it needs them) from 30 before
  // the host's last, so a session outlives its report's worker.
  length = 0;
  const uint32_t frames = getenv_s(&length, buffer, sizeof(buffer), "RENODX_E2E_FRAMES") == 0
                                  && length > 1
                              ? static_cast<uint32_t>(std::strtoul(buffer, nullptr, 10))
                              : 240u;
  issue_report::test_stall_from = frames > 90 ? frames - 30 : 60;
  return true;
}

inline void OnIssueReportTestPresent(reshade::api::effect_runtime* /*runtime*/) {
  using issue_report::Step;
  static uint32_t presents = 0;
  static bool sent = false;
  static uint32_t stalled_ms = 0;
  ++presents;
  const Step at = issue_report::step.load();
  if (presents == 60 && at == Step::kClosed && BeginIssueReport(issue_report::test_reason.c_str())) {
    // The player's own words carry their profile path: the bundle must not.
    char profile[MAX_PATH] = {};
    GetEnvironmentVariableA("USERPROFILE", profile, MAX_PATH);
    std::snprintf(issue_report::typed, sizeof(issue_report::typed),
                  "test report, saved under %s\\Documents", profile);
    issue_report::typed_ticks = 0b10001;
    ContinueIssueReport();
  }
  if (at == Step::kReview && !sent) {
    sent = true;
    SendIssueReport(true);
  }
  if (presents < issue_report::test_stall_from) return;
  // The host's last presents: hold this one until the worker is done (at
  // most two minutes), so the process outlives its report.
  for (Step now = at; (now == Step::kCapturing || now == Step::kCollecting || now == Step::kPacking
                       || now == Step::kUploading || (now == Step::kReview && !sent))
                      && stalled_ms < 120'000;
       now = issue_report::step.load()) {
    if (now == Step::kReview && !sent) {
      sent = true;
      SendIssueReport(true);
    }
    Sleep(20);
    stalled_ms += 20;
  }
}
