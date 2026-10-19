/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The issue report's bundle (rc11): redaction and the zip the report ships
// as.  No addon state here - test/dlss5 runs all of it without a device.
//
// Redaction is by construction, not by review: every text file the bundle
// carries passes RedactText, and the file names are the bundle's own.  What
// is removed:
//   - the Windows profile folder (C:\Users\Example and its 8.3 short form
//     C:\Users\EXAMPL~1 that GetTempPath and old APIs hand out) becomes
//     %USERPROFILE%, whatever the separators (a log may escape '\' as "\\"
//     or write '/') and whatever the case;
//   - the user name anywhere else it stands as a word (D:\<name>\Games, a
//     "user=<name>" field) becomes <user>.
// The F5 PNGs are pixels the addon wrote and carry no path.
//
// The zip is store-only, written by the code below: AGENTS.md rules out a new
// vendored library, and the logs are small.  Entries are streamed from disk
// with the local header patched after the data, so a 4K PNG never sits in
// memory.  Windows' own reader (Expand-Archive) reads it back in test/dlss5.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace renodx::addons::dlss5::report {

inline std::string Utf8(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                        nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>((std::max)(bytes, 0)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), bytes,
                      nullptr, nullptr);
  return out;
}

// ---------------------------------------------------------------------------
// Redaction
// ---------------------------------------------------------------------------

struct Redaction {
  std::string profile;        // "C:\Users\Example UTF-8
  std::string profile_short;  // "C:\Users\EXAMPL~1 or empty when it has none
  std::string user;           // "Name"
};

// The running user's, from the process environment (USERPROFILE and
// USERNAME, what every path the game and ReShade log is built from) and the
// profile's short form.  Environment reads only: safe on any thread.
inline Redaction SystemRedaction() {
  const auto variable = [](const wchar_t* name) {
    std::wstring value(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(),
                                                 static_cast<DWORD>(value.size()));
    value.resize(length < value.size() ? length : 0);
    return value;
  };
  std::wstring profile = variable(L"USERPROFILE");
  while (!profile.empty() && (profile.back() == L'\\' || profile.back() == L'/')) {
    profile.pop_back();
  }
  Redaction redaction{Utf8(profile), {}, Utf8(variable(L"USERNAME"))};
  if (!profile.empty()) {
    std::wstring shortened(32768, L'\0');
    const DWORD length = GetShortPathNameW(profile.c_str(), shortened.data(),
                                           static_cast<DWORD>(shortened.size()));
    if (length != 0 && length < shortened.size()) {
      shortened.resize(length);
      if (_wcsicmp(shortened.c_str(), profile.c_str()) != 0) {
        redaction.profile_short = Utf8(shortened);
      }
    }
  }
  return redaction;
}

inline char AsciiLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

inline bool IsPathSeparator(char c) { return c == '\\' || c == '/'; }

// A character that continues a name: a boundary is anything else.  Non-ASCII
// UTF-8 bytes continue it, so a name never ends inside an accented letter.
inline bool IsNameChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
         || static_cast<unsigned char>(c) >= 0x80;
}

// The length of `path` matched at text[at], 0 when it does not match: ASCII
// case folded, and a run of separators in the text standing for each
// separator of the path.  The match must end at a name boundary, so the
// profile C:\Users\Example never matches C:\Users\ExamplePlus
inline size_t MatchPath(std::string_view text, size_t at, std::string_view path) {
  if (path.empty()) return 0;
  size_t t = at;
  for (size_t p = 0; p < path.size(); ++p) {
    if (t >= text.size()) return 0;
    if (IsPathSeparator(path[p])) {
      if (!IsPathSeparator(text[t])) return 0;
      while (t < text.size() && IsPathSeparator(text[t])) ++t;
      while (p + 1 < path.size() && IsPathSeparator(path[p + 1])) ++p;
      continue;
    }
    if (AsciiLower(text[t]) != AsciiLower(path[p])) return 0;
    ++t;
  }
  return t < text.size() && IsNameChar(text[t]) ? 0 : t - at;
}

inline std::string RedactText(std::string_view text, const Redaction& redaction) {
  std::string out;
  out.reserve(text.size());
  for (size_t at = 0; at < text.size();) {
    size_t matched = MatchPath(text, at, redaction.profile);
    if (matched == 0) matched = MatchPath(text, at, redaction.profile_short);
    if (matched != 0) {
      out += "%USERPROFILE%";
      at += matched;
      continue;
    }
    const std::string_view user = redaction.user;
    if (!user.empty() && (at == 0 || !IsNameChar(text[at - 1]))
        && at + user.size() <= text.size()
        && (at + user.size() == text.size() || !IsNameChar(text[at + user.size()]))
        && std::equal(user.begin(), user.end(), text.begin() + at,
                      [](char a, char b) { return AsciiLower(a) == AsciiLower(b); })) {
      out += "<user>";
      at += user.size();
      continue;
    }
    out.push_back(text[at++]);
  }
  return out;
}

// A file's digest as upper-case hex (BCrypt: BCRYPT_MD5_ALGORITHM for the
// addon's MD5, the key artifacts/dlss5/index.json maps binaries by); empty
// when the file or the provider cannot be opened.
inline std::string FileDigestHex(const std::filesystem::path& path, const wchar_t* algorithm) {
  std::ifstream file(path, std::ios::binary);
  BCRYPT_ALG_HANDLE provider = nullptr;
  if (!file || !BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&provider, algorithm, nullptr, 0))) {
    return {};
  }
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD digest_bytes = 0;
  ULONG written = 0;
  std::string out;
  if (BCRYPT_SUCCESS(BCryptGetProperty(provider, BCRYPT_HASH_LENGTH,
                                       reinterpret_cast<PUCHAR>(&digest_bytes),
                                       sizeof(digest_bytes), &written, 0))
      && BCRYPT_SUCCESS(BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0))) {
    std::vector<char> chunk(1 << 16);
    while (file) {
      file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      if (file.gcount() <= 0) break;
      BCryptHashData(hash, reinterpret_cast<PUCHAR>(chunk.data()),
                     static_cast<ULONG>(file.gcount()), 0);
    }
    std::vector<uint8_t> digest(digest_bytes);
    if (BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), digest_bytes, 0))) {
      for (const uint8_t byte : digest) {
        out.push_back("0123456789ABCDEF"[byte >> 4]);
        out.push_back("0123456789ABCDEF"[byte & 15]);
      }
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(provider, 0);
  return out;
}

// ---------------------------------------------------------------------------
// report.json
// ---------------------------------------------------------------------------

struct TextExcerpt {
  std::string text;
  uint64_t file_bytes = 0;
  uint64_t omitted_bytes = 0;
};

// Keep the startup contract and the events nearest the report together. A
// tail-only excerpt hides loader, hook and route decisions in long sessions.
inline TextExcerpt ReadTextExcerpt(const std::filesystem::path& path, size_t limit,
                                   size_t startup_bytes = 256u << 10) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in || limit == 0) return {};
  const std::streamoff end = in.tellg();
  if (end < 0) return {};
  TextExcerpt result;
  result.file_bytes = static_cast<uint64_t>(end);
  const auto read_at = [&](uint64_t offset, size_t bytes) {
    std::string part(bytes, '\0');
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(part.data(), static_cast<std::streamsize>(part.size()));
    part.resize(static_cast<size_t>((std::max)(in.gcount(), std::streamsize(0))));
    return part;
  };
  if (result.file_bytes <= limit) {
    result.text = read_at(0, static_cast<size_t>(result.file_bytes));
  } else {
    const size_t head = (std::min)(startup_bytes, limit / 2);
    const size_t tail = limit - head;
    result.omitted_bytes = result.file_bytes - head - tail;
    result.text = read_at(0, head) + "\n[middle " + std::to_string(result.omitted_bytes)
                  + " bytes omitted from source file]\n"
                  + read_at(result.file_bytes - tail, tail);
  }
  return result;
}

inline std::string JsonString(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
          out += escaped;
        } else {
          out.push_back(c);
        }
    }
  }
  return out + "\"";
}

// ---------------------------------------------------------------------------
// Zip (PKWARE APPNOTE 6.3: store method, UTF-8 names, no zip64)
// ---------------------------------------------------------------------------

inline const std::array<uint32_t, 256>& Crc32Table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> out{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
      out[i] = crc;
    }
    return out;
  }();
  return table;
}

// Streaming CRC-32: start from 0, feed chunks in order.
inline uint32_t Crc32(uint32_t crc, const void* data, size_t size) {
  const auto& table = Crc32Table();
  crc = ~crc;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

class ZipWriter {
 public:
  // Store-only zips cap every size at 4 GiB - 1; a bundle is far below.
  static constexpr uint64_t kMaxBytes = 0xFFFFFFFEull;

  explicit ZipWriter(const std::filesystem::path& path)
      : out_(path, std::ios::binary | std::ios::trunc) {
    ok_ = out_.good();
  }

  bool Ok() const { return ok_; }
  uint64_t Bytes() const { return offset_; }

  bool AddBytes(std::string_view name, const void* data, size_t size) {
    if (!Begin(name)) return false;
    Entry& entry = entries_.back();
    entry.crc = Crc32(0, data, size);
    entry.size = static_cast<uint32_t>(size);
    out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return End(size);
  }

  bool AddFile(std::string_view name, const std::filesystem::path& source) {
    // A source that cannot be opened is skipped; a failed write poisons
    // the zip (Begin/End clear ok_).
    std::ifstream in(source, std::ios::binary);
    if (!in || !Begin(name)) return false;
    std::vector<char> chunk(1 << 20);
    uint64_t size = 0;
    uint32_t crc = 0;
    while (in) {
      in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      const std::streamsize read = in.gcount();
      if (read <= 0) break;
      crc = Crc32(crc, chunk.data(), static_cast<size_t>(read));
      out_.write(chunk.data(), read);
      size += static_cast<uint64_t>(read);
    }
    Entry& entry = entries_.back();
    entry.crc = crc;
    entry.size = static_cast<uint32_t>(size);
    return End(size);
  }

  // The central directory.  The zip is complete only after this returns true.
  bool Finish() {
    if (!ok_) return false;
    const uint64_t directory_offset = offset_;
    std::string directory;
    for (const Entry& entry : entries_) {
      AppendLe32(&directory, 0x02014B50u);
      AppendLe16(&directory, 20);  // made by: 2.0, MS-DOS attributes
      AppendLe16(&directory, 20);  // needed: 2.0
      AppendLe16(&directory, kUtf8Flag);
      AppendLe16(&directory, 0);  // stored
      AppendLe16(&directory, entry.dos_time);
      AppendLe16(&directory, entry.dos_date);
      AppendLe32(&directory, entry.crc);
      AppendLe32(&directory, entry.size);
      AppendLe32(&directory, entry.size);
      AppendLe16(&directory, static_cast<uint16_t>(entry.name.size()));
      AppendLe16(&directory, 0);  // extra
      AppendLe16(&directory, 0);  // comment
      AppendLe16(&directory, 0);  // disk
      AppendLe16(&directory, 0);  // internal attributes
      AppendLe32(&directory, 0);  // external attributes
      AppendLe32(&directory, entry.offset);
      directory += entry.name;
    }
    std::string end;
    AppendLe32(&end, 0x06054B50u);
    AppendLe16(&end, 0);
    AppendLe16(&end, 0);
    AppendLe16(&end, static_cast<uint16_t>(entries_.size()));
    AppendLe16(&end, static_cast<uint16_t>(entries_.size()));
    AppendLe32(&end, static_cast<uint32_t>(directory.size()));
    AppendLe32(&end, static_cast<uint32_t>(directory_offset));
    AppendLe16(&end, 0);
    out_.write(directory.data(), static_cast<std::streamsize>(directory.size()));
    out_.write(end.data(), static_cast<std::streamsize>(end.size()));
    offset_ += directory.size() + end.size();
    out_.flush();
    ok_ = out_.good() && offset_ <= kMaxBytes && entries_.size() < 0xFFFF;
    out_.close();
    return ok_;
  }

 private:
  static constexpr uint16_t kUtf8Flag = 0x0800;

  struct Entry {
    std::string name;
    uint32_t offset = 0;
    uint32_t crc = 0;
    uint32_t size = 0;
    uint16_t dos_time = 0;
    uint16_t dos_date = 0;
  };

  static void AppendLe16(std::string* out, uint16_t value) {
    out->push_back(static_cast<char>(value & 0xFF));
    out->push_back(static_cast<char>(value >> 8));
  }
  static void AppendLe32(std::string* out, uint32_t value) {
    AppendLe16(out, static_cast<uint16_t>(value & 0xFFFF));
    AppendLe16(out, static_cast<uint16_t>(value >> 16));
  }

  // Writes the local header with the CRC and sizes zeroed; End patches them.
  bool Begin(std::string_view name) {
    if (!ok_ || name.empty() || name.size() > 0xFFFF || offset_ > kMaxBytes) return ok_ = false;
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    Entry entry;
    entry.name = name;
    entry.offset = static_cast<uint32_t>(offset_);
    entry.dos_time = static_cast<uint16_t>((local.tm_hour << 11) | (local.tm_min << 5)
                                           | (local.tm_sec / 2));
    entry.dos_date = static_cast<uint16_t>(((std::max)(local.tm_year - 80, 0) << 9)
                                           | ((local.tm_mon + 1) << 5) | local.tm_mday);
    std::string header;
    AppendLe32(&header, 0x04034B50u);
    AppendLe16(&header, 20);
    AppendLe16(&header, kUtf8Flag);
    AppendLe16(&header, 0);
    AppendLe16(&header, entry.dos_time);
    AppendLe16(&header, entry.dos_date);
    AppendLe32(&header, 0);  // crc, patched
    AppendLe32(&header, 0);  // compressed size, patched
    AppendLe32(&header, 0);  // size, patched
    AppendLe16(&header, static_cast<uint16_t>(name.size()));
    AppendLe16(&header, 0);
    header += name;
    out_.write(header.data(), static_cast<std::streamsize>(header.size()));
    offset_ += header.size();
    entries_.push_back(std::move(entry));
    return ok_ = out_.good();
  }

  bool End(uint64_t size) {
    const Entry& entry = entries_.back();
    if (size > kMaxBytes || offset_ + size > kMaxBytes) return ok_ = false;
    std::string patch;
    AppendLe32(&patch, entry.crc);
    AppendLe32(&patch, entry.size);
    AppendLe32(&patch, entry.size);
    const std::streamoff resume = out_.tellp();
    out_.seekp(static_cast<std::streamoff>(entry.offset) + 14);
    out_.write(patch.data(), static_cast<std::streamsize>(patch.size()));
    out_.seekp(resume);
    offset_ += size;
    return ok_ = out_.good();
  }

  std::ofstream out_;
  std::vector<Entry> entries_;
  uint64_t offset_ = 0;
  bool ok_ = false;
};

}  // namespace renodx::addons::dlss5::report
