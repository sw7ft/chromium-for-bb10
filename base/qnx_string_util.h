#ifndef BASE_QNX_STRING_UTIL_H_
#define BASE_QNX_STRING_UTIL_H_

#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)

#include <cstddef>
#include <string>

namespace base {
namespace qnx {

inline size_t FindFirstOf(const char* data, size_t len, const char* chars,
                          size_t start = 0) {
  for (size_t i = start; i < len; i++) {
    for (const char* c = chars; *c; c++) {
      if (data[i] == *c)
        return i;
    }
  }
  return std::string::npos;
}

inline size_t FindFirstNotOf(const char* data, size_t len, const char* chars,
                             size_t start = 0) {
  for (size_t i = start; i < len; i++) {
    bool found = false;
    for (const char* c = chars; *c; c++) {
      if (data[i] == *c) {
        found = true;
        break;
      }
    }
    if (!found)
      return i;
  }
  return std::string::npos;
}

inline size_t FindChar(const char* data, size_t len, char target,
                       size_t start = 0) {
  for (size_t i = start; i < len; i++) {
    if (data[i] == target)
      return i;
  }
  return std::string::npos;
}

inline size_t StrFind(const std::string& s, char c, size_t pos = 0) {
  for (size_t i = pos; i < s.size(); i++) {
    if (s[i] == c)
      return i;
  }
  return std::string::npos;
}

}  // namespace qnx
}  // namespace base

#endif  // BUILDFLAG(IS_QNX)

#endif  // BASE_QNX_STRING_UTIL_H_
