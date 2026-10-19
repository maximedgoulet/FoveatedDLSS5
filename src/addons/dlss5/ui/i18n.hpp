/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// The panel's translations (v7.0.0).
//
// A string is translated where it is drawn: Tr("English") returns the
// current language's text, or the English it was given when the language
// has none.  The English text is the key, so the source keeps reading as
// English and a string no table knows yet still draws.  Only what a player
// reads is translated: the modern layout, the status card, the HUD pill and
// the welcome.  Log lines, the NR-CARD/NR-VERDICT lines, the support report,
// Raw details and the Developer view's own controls stay English - they are
// what a support thread reads, in one language - and so do the Language
// row's choices (DrawSectionLanguage).
//
// The tables live in ui/lang/<code>.hpp, written by tools/i18n/ui_strings.py
// from reviewed JSON; the same tool lists the English strings the panel
// draws and checks every table against them.  A translation whose printf
// conversions differ from its English (count, order or type) is dropped when
// the table is loaded, so a bad entry falls back to English instead of
// reading the wrong argument.
//
// The language is ReShade's own: ReShade sets its UI language on the
// overlay thread while add-ons draw (runtime_gui.cpp, set_current_language),
// and loads the font for that language only.  Following it keeps every
// glyph drawable; UiLanguage in [RenoDX.DLSS5] overrides it.  This header is
// platform-neutral - dlssnr.hpp reads the thread's language.

#pragma once

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace renodx::addons::dlss5::ui {

struct Translation {
  const char* en;
  const char* text;
};

}  // namespace renodx::addons::dlss5::ui

#include "lang/bg.hpp"
#include "lang/de.hpp"
#include "lang/es.hpp"
#include "lang/fr.hpp"
#include "lang/it.hpp"
#include "lang/ja.hpp"
#include "lang/ko.hpp"
#include "lang/pt-BR.hpp"
#include "lang/ru.hpp"
#include "lang/sl.hpp"
#include "lang/th.hpp"
#include "lang/tr.hpp"
#include "lang/zh-Hans.hpp"
#include "lang/zh-Hant.hpp"

namespace renodx::addons::dlss5::ui {

struct Language {
  const char* code;  // what UiLanguage stores
  const char* name;  // English; drawn through Tr like any other label
  std::span<const Translation> table;
};

// ReShade's own UI languages plus Italian (Latin-1, which ReShade's default
// font covers).  English is first; a language is matched by code.
inline constexpr Language kLanguages[] = {
    {"en", "English", {}},
    {"de", "German", lang::kDe},
    {"fr", "French", lang::kFr},
    {"es", "Spanish", lang::kEs},
    {"it", "Italian", lang::kIt},
    {"pt-BR", "Portuguese (Brazil)", lang::kPtBr},
    {"ru", "Russian", lang::kRu},
    {"bg", "Bulgarian", lang::kBg},
    {"sl", "Slovenian", lang::kSl},
    {"tr", "Turkish", lang::kTr},
    {"th", "Thai", lang::kTh},
    {"ja", "Japanese", lang::kJa},
    {"ko", "Korean", lang::kKo},
    {"zh-Hans", "Chinese (Simplified)", lang::kZhHans},
    {"zh-Hant", "Chinese (Traditional)", lang::kZhHant},
};
inline constexpr int kLanguageCount = static_cast<int>(std::size(kLanguages));

// The printf conversions of `text`, in order, as written ("%.2f", "%llu");
// "%%" is text, not a conversion.  Two strings may stand in for each other
// as a format only when these match.
inline std::string FormatConversions(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') continue;
    if (i + 1 < text.size() && text[i + 1] == '%') {
      ++i;
      continue;
    }
    const std::size_t start = i++;
    while (i < text.size()
           && std::string_view("-+ #0123456789.*hljztL").find(text[i])
                  != std::string_view::npos) {
      ++i;
    }
    out.append(text.substr(start, i + 1 - start));
    out += '|';
  }
  return out;
}

// The language whose tag `tag` names ("de-DE", "pt-PT", "zh-TW",
// "zh-Hans-CN"), by its primary subtag; Chinese splits by script and
// region.  0 (English) when none matches.
inline int MatchLanguage(std::string_view tag) {
  std::string lower(tag);
  for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  const std::string_view primary = std::string_view(lower).substr(0, lower.find('-'));
  if (primary == "zh") {
    // ReShade's own split (runtime_gui.cpp): Hant, TW and HK load the
    // Traditional font, everything else the Simplified one.
    const bool traditional = lower.find("hant") != std::string::npos
                             || lower.find("-tw") != std::string::npos
                             || lower.find("-hk") != std::string::npos;
    return traditional ? kLanguageCount - 1 : kLanguageCount - 2;
  }
  for (int i = 0; i < kLanguageCount; ++i) {
    std::string code(kLanguages[i].code);
    for (char& c : code) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (code == lower || code.substr(0, code.find('-')) == primary) return i;
  }
  return 0;
}

namespace i18n_internal {
// -1 until the first SelectLanguage, so the first overlay frame logs the
// language even when it is English.
inline int current = -1;
inline std::unordered_map<std::string_view, const char*> table;
inline int dropped = 0;
// ReShade draws each runtime's overlay on the thread that presents it; a
// game with two swapchains on two threads would otherwise rebuild the table
// under the other's lookups.  Uncontended in every other case.
inline std::shared_mutex lock;
}  // namespace i18n_internal

inline int CurrentLanguage() { return (std::max)(i18n_internal::current, 0); }

// Entries dropped when the current language loaded: printf conversions that
// differ from the English.  The language line in ReShade.log states it.
inline int DroppedTranslations() { return i18n_internal::dropped; }

// Switches the tables.  Returns true when the language changed.
inline bool SelectLanguage(int index) {
  using namespace i18n_internal;
  if (index < 0 || index >= kLanguageCount) index = 0;
  std::unique_lock guard(lock);
  if (index == current) return false;
  current = index;
  table.clear();
  dropped = 0;
  for (const Translation& entry : kLanguages[index].table) {
    if (FormatConversions(entry.en) != FormatConversions(entry.text)) {
      ++dropped;
      continue;
    }
    table.emplace(entry.en, entry.text);
  }
  return true;
}

// `english` in the current language, or `english` itself.
inline const char* Tr(const char* english) {
  if (english == nullptr) return english;
  std::shared_lock guard(i18n_internal::lock);
  if (i18n_internal::current <= 0) return english;
  const auto found = i18n_internal::table.find(english);
  return found == i18n_internal::table.end() ? english : found->second;
}

}  // namespace renodx::addons::dlss5::ui
