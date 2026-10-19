/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The issue report's upload (rc11): an anonymous temporary file host, over
// WinHTTP, from a thread of its own.  Nothing here runs unless the player
// clicked "Upload & copy link" in the review step.
//
// The hosts, as their public pages state them (checked 2026-09-24):
//   litterbox  https://litterbox.catbox.moe/tools.php: POST multipart to
//              /resources/internals/api.php with reqtype=fileupload,
//              time=1h|12h|24h|72h and fileToUpload; "Temporary uploads up
//              to 1 GB"; the body is the file's URL (the site's ShareX
//              config: ResponseType Text).  The FAQ: "Your IP address is
//              stored with your uploads", and .exe/.scr/.cpl/.doc/.jar are
//              refused - a .zip is not.
//   tempsh     https://temp.sh/: `curl -F "file=@test.txt"
//              https://temp.sh/upload`; "Files expire after 3 days";
//              "Current file size limit is 4GB".  The page documents no
//              response format; the body is read as the URL, like curl
//              prints it, and anything that is not one is a failure.
// Both keep a file 72 hours.  The list is data (NRReportHost, ini-only):
// "litterbox,tempsh" by default, tried in order; "none" saves the zip only.
// A dead host is swapped by editing the ini, never by a rebuild.
//
// The link is the secret: anyone holding it can download the zip.  It goes
// to the clipboard and the panel, never to a log line (LinkForLog keeps the
// scheme and host only).
//
// RENODX_NR_TEST_REPORT_URL (test only) replaces every host with that URL,
// posted in litterbox's form shape: the e2e lane points it at a loopback
// server that checks the multipart form and answers with a fake link.  No
// test ever reaches a real host.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")
// Loaded on the first upload, with /DELAYLOAD:WINHTTP.dll on the CMake target.
#pragma comment(lib, "delayimp.lib")

namespace renodx::addons::dlss5::report {

struct Host {
  const char* id;       // the NRReportHost token
  const char* display;  // what the review step names and the log states
  const wchar_t* url;
  const char* file_field;
  std::array<std::pair<const char*, const char*>, 2> fields;  // before the file
  uint32_t retention_hours;
  uint64_t limit_bytes;
};

inline constexpr Host kHosts[] = {
    {"litterbox", "litterbox.catbox.moe",
     L"https://litterbox.catbox.moe/resources/internals/api.php", "fileToUpload",
     {{{"reqtype", "fileupload"}, {"time", "72h"}}}, 72, 1'000'000'000ull},
    {"tempsh", "temp.sh", L"https://temp.sh/upload", "file", {{{nullptr, nullptr}}}, 72,
     4'000'000'000ull},
};

inline constexpr char kDefaultHosts[] = "litterbox,tempsh";

// NRReportHost as written: tokens split on ',', ';', white space and the
// '\0' ReShade's ini reader joins comma-split elements with.  "none" means
// save only and ends the list.  Unknown tokens are appended to `unknown`
// for the log; an empty value is the default list.
inline std::vector<const Host*> ParseHosts(std::string_view raw, std::string* unknown) {
  std::vector<const Host*> hosts;
  bool any = false;
  for (size_t at = 0; at <= raw.size();) {
    size_t end = at;
    while (end < raw.size() && raw[end] != ',' && raw[end] != ';' && raw[end] != ' '
           && raw[end] != '\t' && raw[end] != '\0') {
      ++end;
    }
    std::string token(raw.substr(at, end - at));
    for (char& c : token) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    at = end + 1;
    if (token.empty()) continue;
    any = true;
    if (token == "none") return {};
    if (token == "temp.sh") token = "tempsh";
    bool known = false;
    for (const Host& host : kHosts) {
      if (token != host.id) continue;
      known = true;
      if (std::find(hosts.begin(), hosts.end(), &host) == hosts.end()) hosts.push_back(&host);
    }
    if (!known && unknown != nullptr) *unknown += (unknown->empty() ? "" : ",") + token;
  }
  return any ? hosts : ParseHosts(kDefaultHosts, nullptr);
}

// ---------------------------------------------------------------------------
// multipart/form-data (RFC 7578): the host's fields, then the file part.
// The file's bytes go between MultipartHead and MultipartTail.
// ---------------------------------------------------------------------------

inline std::string MultipartHead(std::string_view boundary, const Host& host,
                                 std::string_view file_name) {
  std::string head;
  for (const auto& [name, value] : host.fields) {
    if (name == nullptr) continue;
    head.append("--").append(boundary).append("\r\nContent-Disposition: form-data; name=\"");
    head.append(name).append("\"\r\n\r\n").append(value).append("\r\n");
  }
  head.append("--").append(boundary).append("\r\nContent-Disposition: form-data; name=\"");
  head.append(host.file_field).append("\"; filename=\"").append(file_name);
  head.append("\"\r\nContent-Type: application/zip\r\n\r\n");
  return head;
}

inline std::string MultipartTail(std::string_view boundary) {
  return std::string("\r\n--").append(boundary).append("--\r\n");
}

// 32 random hex digits after a fixed prefix: a zip that happens to hold the
// boundary is a 2^-128 event.
inline std::string MultipartBoundary() {
  uint8_t random[16] = {};
  BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  std::string boundary = "----RenoDXReport";
  for (const uint8_t byte : random) {
    boundary.push_back("0123456789abcdef"[byte >> 4]);
    boundary.push_back("0123456789abcdef"[byte & 15]);
  }
  return boundary;
}

// A host's answer is a link only when it is one https URL and nothing else.
inline bool IsLink(std::string_view text) {
  if (text.size() < 12 || text.size() > 512 || text.substr(0, 8) != "https://") return false;
  for (const char c : text) {
    if (c <= ' ' || c > '~' || c == '"' || c == '<' || c == '>' || c == '\\') return false;
  }
  return true;
}

// What a log may say about a link: the scheme and host, never the path.
inline std::string LinkForLog(std::string_view link) {
  const size_t host_end = link.find('/', link.find("://") == std::string_view::npos
                                             ? 0
                                             : link.find("://") + 3);
  return std::string(link.substr(0, host_end)) + "/<redacted>";
}

struct UploadProgress {
  std::atomic_uint64_t sent{0};
  std::atomic_uint64_t total{0};
  std::atomic_bool cancel{false};
};

struct UploadResult {
  bool ok = false;
  bool cancelled = false;
  std::string link;   // the secret: clipboard and panel only
  std::string error;  // plain words and a code, for the panel and the log
  unsigned long status = 0;
};

inline std::string DescribeWinHttpError(DWORD code) {
  const char* what = "network error";
  switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: what = "the host name did not resolve (offline, or DNS blocked)"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT: what = "could not connect (offline, a firewall or a proxy)"; break;
    case ERROR_WINHTTP_TIMEOUT: what = "the connection timed out"; break;
    case ERROR_WINHTTP_CONNECTION_ERROR: what = "the connection was reset"; break;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
      what = "the secure (TLS) connection failed; a proxy or antivirus may intercept it";
      break;
    case ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR:
    case ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT: what = "the automatic proxy setup failed"; break;
    default: break;
  }
  char text[160];
  std::snprintf(text, sizeof(text), "%s (WinHTTP %lu)", what, static_cast<unsigned long>(code));
  return text;
}

// One upload, synchronous: call it from a thread of its own.  `progress`
// counts bytes sent; its cancel flag is read between 64 KB chunks.  Uses
// WinHTTP's automatic proxy (WPAD/PAC, Windows 8.1+) and falls back to the
// system's static proxy setting on older systems.  TLS 1.2 or newer.
inline UploadResult Upload(const Host& host, const std::wstring& url,
                           const std::filesystem::path& zip, const std::wstring& user_agent,
                           UploadProgress* progress) {
  UploadResult result;
  std::ifstream file(zip, std::ios::binary);
  std::error_code size_error;
  const uint64_t file_bytes = std::filesystem::file_size(zip, size_error);
  if (!file || size_error) {
    result.error = "the zip could not be read";
    return result;
  }
  if (file_bytes > host.limit_bytes) {
    result.error = "the zip is larger than the host accepts";
    return result;
  }
  wchar_t host_name[256] = {};
  wchar_t path[2048] = {};
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.lpszHostName = host_name;
  parts.dwHostNameLength = static_cast<DWORD>(std::size(host_name));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
    result.error = "the host URL is malformed";
    return result;
  }
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

  HINTERNET session = WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) {
    session = WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  HINTERNET connection = nullptr;
  HINTERNET request = nullptr;
  const auto fail = [&](DWORD code) {
    result.error = DescribeWinHttpError(code);
    if (request != nullptr) WinHttpCloseHandle(request);
    if (connection != nullptr) WinHttpCloseHandle(connection);
    if (session != nullptr) WinHttpCloseHandle(session);
    return result;
  };
  if (session == nullptr) return fail(GetLastError());
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
  if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                        sizeof(protocols))) {
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // before Windows 11: no TLS 1.3 flag
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  }
  WinHttpSetTimeouts(session, 15'000, 15'000, 60'000, 60'000);
  connection = WinHttpConnect(session, host_name, parts.nPort, 0);
  if (connection == nullptr) return fail(GetLastError());
  request = WinHttpOpenRequest(connection, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
  if (request == nullptr) return fail(GetLastError());

  const std::string boundary = MultipartBoundary();
  const std::string head = MultipartHead(boundary, host, "renodx-dlss5-report.zip");
  const std::string tail = MultipartTail(boundary);
  const uint64_t total = head.size() + file_bytes + tail.size();
  progress->total = total;
  progress->sent = 0;
  const std::wstring headers = L"Content-Type: multipart/form-data; boundary="
                               + std::wstring(boundary.begin(), boundary.end());
  if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L),
                          WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(total), 0)) {
    return fail(GetLastError());
  }
  const auto write = [&](const char* data, size_t size) {
    DWORD written = 0;
    if (!WinHttpWriteData(request, data, static_cast<DWORD>(size), &written) || written != size) {
      return false;
    }
    progress->sent += written;
    return true;
  };
  if (!write(head.data(), head.size())) return fail(GetLastError());
  std::vector<char> chunk(64 * 1024);
  while (file) {
    if (progress->cancel.load(std::memory_order_relaxed)) {
      result.cancelled = true;
      return fail(ERROR_WINHTTP_OPERATION_CANCELLED);
    }
    file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize read = file.gcount();
    if (read <= 0) break;
    if (!write(chunk.data(), static_cast<size_t>(read))) return fail(GetLastError());
  }
  if (!write(tail.data(), tail.size())) return fail(GetLastError());
  if (!WinHttpReceiveResponse(request, nullptr)) return fail(GetLastError());

  DWORD status = 0;
  DWORD status_bytes = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_bytes,
                      WINHTTP_NO_HEADER_INDEX);
  result.status = status;
  std::string body;
  for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available != 0
                            && body.size() < 16 * 1024;) {
    std::string part(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request, part.data(), available, &read) || read == 0) break;
    body.append(part.data(), read);
  }
  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);

  while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) {
    body.pop_back();
  }
  while (!body.empty() && (body.front() == '\n' || body.front() == '\r' || body.front() == ' ')) {
    body.erase(body.begin());
  }
  const std::string first_line = body.substr(0, body.find_first_of("\r\n"));
  if (status >= 200 && status < 300 && IsLink(first_line)) {
    result.ok = true;
    result.link = first_line;
    return result;
  }
  // The host's own error text, first line, printable ASCII only - and never
  // anything link-shaped, which may be a working link after all.
  std::string said;
  for (const char c : first_line) {
    if (said.size() >= 120) break;
    if (c >= ' ' && c <= '~') said.push_back(c);
  }
  if (said.find("://") != std::string::npos) said = "an answer that is not a single link";
  char text[256];
  std::snprintf(text, sizeof(text), "the host answered HTTP %lu%s%s",
                static_cast<unsigned long>(status), said.empty() ? "" : ": ", said.c_str());
  result.error = text;
  return result;
}

}  // namespace renodx::addons::dlss5::report
