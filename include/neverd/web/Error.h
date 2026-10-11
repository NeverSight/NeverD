//===- Error.h - Offline analysis diagnostic contract ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed diagnostic codes shared by artifact readers and analysis consumers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_ERROR_H
#define NEVERD_WEB_ERROR_H

#include <stdexcept>

namespace neverd::web {

/// Input paths, source text and exception details must never be interpolated
/// into ordinary API failures. This contract is independent of sessions.
class Error : public std::runtime_error {
public:
  explicit Error(const char *Code) : std::runtime_error(Code) {}
};

} // namespace neverd::web

#endif // NEVERD_WEB_ERROR_H
