// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_TYPES_SUPPORTS_OSTREAM_OPERATOR_H_
#define BASE_TYPES_SUPPORTS_OSTREAM_OPERATOR_H_

#include <ostream>
#include <type_traits>
#include <utility>

namespace base::internal {

// Detects whether using operator<< would work.
//
// Note that the above #include of <ostream> is necessary to guarantee
// consistent results here for basic types.
#if __cplusplus >= 202002L
template <typename T>
concept SupportsOstreamOperator =
    requires(const T& t, std::ostream& os) { os << t; };
#else
// C++17 SFINAE fallback for concept SupportsOstreamOperator.
template <typename T, typename = void>
struct SupportsOstreamOperatorImpl : std::false_type {};

template <typename T>
struct SupportsOstreamOperatorImpl<
    T,
    std::void_t<decltype(std::declval<std::ostream&>()
                         << std::declval<const T&>())>> : std::true_type {};

template <typename T>
inline constexpr bool SupportsOstreamOperator =
    SupportsOstreamOperatorImpl<T>::value;
#endif

}  // namespace base::internal

#endif  // BASE_TYPES_SUPPORTS_OSTREAM_OPERATOR_H_
