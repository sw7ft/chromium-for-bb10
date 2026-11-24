// Copyright 2026 SW7FT. All rights reserved.
// Turns text another app shared to the browser into something to open.

#ifndef CONTENT_SHELL_BROWSER_BERRY_SHARED_TEXT_H_
#define CONTENT_SHELL_BROWSER_BERRY_SHARED_TEXT_H_

#include <cstddef>
#include <string>

namespace content {

// Standard-library only, so the logic can be checked on the build host
// (deploy/berrybench/shared_text_test.cc).

namespace berry_shared_text_internal {

inline bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

inline char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool StartsWithNoCase(const std::string& s, size_t pos, const char* p) {
  for (size_t i = 0; p[i]; ++i) {
    if (pos + i >= s.size() || Lower(s[pos + i]) != p[i])
      return false;
  }
  return true;
}

}  // namespace berry_shared_text_internal

// Returns the first http:// or https:// link in |text|, or an empty string.
//
// Apps share a link either on its own or inside a sentence ("Look at this:
// https://example.com/a."), so the link ends at the first whitespace or quote
// and punctuation that closes the sentence around it is not part of it.
inline std::string BerryFirstLinkInText(const std::string& text) {
  using namespace berry_shared_text_internal;
  for (size_t i = 0; i < text.size(); ++i) {
    if (Lower(text[i]) != 'h')
      continue;
    size_t scheme_len = 0;
    if (StartsWithNoCase(text, i, "https://"))
      scheme_len = 8;
    else if (StartsWithNoCase(text, i, "http://"))
      scheme_len = 7;
    else
      continue;
    // "xhttp://" is not a link, "(http://" is.
    if (i > 0) {
      const char prev = text[i - 1];
      if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
          (prev >= '0' && prev <= '9'))
        continue;
    }
    size_t end = i + scheme_len;
    while (end < text.size()) {
      const unsigned char c = static_cast<unsigned char>(text[end]);
      if (c <= 0x20 || c == 0x7f || c == '"' || c == '<' || c == '>' ||
          c == '`')
        break;
      ++end;
    }
    while (end > i + scheme_len) {
      const char last = text[end - 1];
      if (last == '.' || last == ',' || last == ';' || last == ':' ||
          last == '!' || last == '?' || last == '\'') {
        --end;
        continue;
      }
      // A closing bracket belongs to the link only if it was opened inside it
      // (https://en.wikipedia.org/wiki/Passport_(phone)).
      if (last == ')' || last == ']') {
        const char open = last == ')' ? '(' : '[';
        int depth = 0;
        for (size_t k = i; k < end; ++k) {
          if (text[k] == open)
            ++depth;
          else if (text[k] == last)
            --depth;
        }
        if (depth < 0) {
          --end;
          continue;
        }
      }
      break;
    }
    if (end > i + scheme_len)
      return text.substr(i, end - i);
  }
  return std::string();
}

// Shared text with no link in it, tidied into one line that can be handed to
// the same code as text typed in the address bar (a bare host name is opened,
// anything else is searched for). Cut to |max_len| bytes at a character
// boundary; a search for a whole shared paragraph helps nobody.
inline std::string BerrySharedTextAsQuery(const std::string& text,
                                          size_t max_len = 200) {
  using namespace berry_shared_text_internal;
  std::string out;
  bool pending_space = false;
  for (char c : text) {
    if (IsSpace(c) || static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out += ' ';
      pending_space = false;
    }
    out += c;
  }
  if (out.size() > max_len) {
    size_t cut = max_len;
    // Do not split a UTF-8 sequence.
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
      --cut;
    out.resize(cut);
    while (!out.empty() && out.back() == ' ')
      out.pop_back();
  }
  return out;
}

}  // namespace content

#endif  // CONTENT_SHELL_BROWSER_BERRY_SHARED_TEXT_H_
