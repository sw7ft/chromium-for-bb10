// Host-side check of how text shared from another app becomes a link.
//
//   g++ -std=c++17 -I../.. shared_text_test.cc -o /tmp/shared_text_test && /tmp/shared_text_test
#include <cstdio>
#include <string>

#include "content/shell/browser/berry_shared_text.h"

namespace {

int g_failed = 0;

void Check(const char* what, const std::string& got, const std::string& want) {
  const bool ok = got == want;
  g_failed += !ok;
  std::printf("%-4s %-34s [%s]%s%s%s\n", ok ? "ok" : "FAIL", what, got.c_str(),
              ok ? "" : "  want [", ok ? "" : want.c_str(), ok ? "" : "]");
}

void Link(const char* what, const std::string& text, const std::string& want) {
  Check(what, content::BerryFirstLinkInText(text), want);
}

}  // namespace

int main() {
  Link("bare link", "https://example.com/a?b=1#c", "https://example.com/a?b=1#c");
  Link("link in a sentence", "Look at this: https://example.com/a. Nice!",
       "https://example.com/a");
  Link("title then link on next line", "BlackBerry news\nhttp://example.com/x\n",
       "http://example.com/x");
  Link("link in brackets", "(see https://example.com/a)", "https://example.com/a");
  Link("brackets inside the link",
       "https://en.wikipedia.org/wiki/Passport_(phone)",
       "https://en.wikipedia.org/wiki/Passport_(phone)");
  Link("brackets inside, then closed",
       "(https://en.wikipedia.org/wiki/Passport_(phone))",
       "https://en.wikipedia.org/wiki/Passport_(phone)");
  Link("quoted", "\"https://example.com/a\"", "https://example.com/a");
  Link("angle brackets", "<https://example.com/a>", "https://example.com/a");
  Link("upper-case scheme", "HTTPS://Example.com/A", "HTTPS://Example.com/A");
  Link("first of two", "http://a.example/1 and https://b.example/2",
       "http://a.example/1");
  Link("trailing question mark", "Have you seen https://example.com/a?",
       "https://example.com/a");
  Link("query kept", "https://example.com/a?q=1&r=2,", "https://example.com/a?q=1&r=2");
  Link("no link", "just some words", "");
  Link("scheme only", "https:// nothing", "");
  Link("other scheme", "file:///etc/passwd javascript://x", "");
  Link("glued to a word", "xhttps://example.com", "");
  Link("empty", "", "");

  Check("query: tidy whitespace",
        content::BerrySharedTextAsQuery("  hello\r\n  big\tworld \n"),
        "hello big world");
  Check("query: bare host survives",
        content::BerrySharedTextAsQuery(" example.com\n"), "example.com");
  Check("query: cut to length",
        content::BerrySharedTextAsQuery("abcdef ghijkl", 8), "abcdef g");
  Check("query: cut ends on a space",
        content::BerrySharedTextAsQuery("abcdef ghijkl", 7), "abcdef");
  // "é" is two bytes; the cut must not leave half of it.
  Check("query: utf-8 boundary",
        content::BerrySharedTextAsQuery("ab\xc3\xa9\xc3\xa9", 5), "ab\xc3\xa9");
  Check("query: empty", content::BerrySharedTextAsQuery(" \n\t"), "");

  std::printf("shared text: %s\n", g_failed ? "FAILED" : "all passed");
  return g_failed ? 1 : 0;
}
