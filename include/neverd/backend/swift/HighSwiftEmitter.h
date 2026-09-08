//===- HighSwiftEmitter.h - HighIR to actual Swift bodies ---------*- C++
//-*-===//
#ifndef NEVERD_BACKEND_SWIFT_HIGHSWIFTEMITTER_H
#define NEVERD_BACKEND_SWIFT_HIGHSWIFTEMITTER_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/loader/Swift/SwiftMethods.h"

#include <string>
#include <vector>

namespace neverd {

struct SwiftEmissionResult {
  bool Recovered = false;
  std::string Source;
  /// Member text lets a module assembler place designated initializers in
  /// their class definition rather than an invalid Swift extension.
  std::string MemberSource;
  std::string Reason;
  std::vector<std::string> Limitations;
  std::vector<va_t> Dependencies;
};

/// Emit one function or an extension containing one actual method body.
/// The caller must validate decoding/lifting audits and apply the source hint
/// before invoking this writer. Unknown IR never becomes a placeholder body.
class HighSwiftEmitter {
public:
  SwiftEmissionResult
  emit(const HighFunc &Function, const SwiftSourceSignature &Signature,
       const std::vector<SwiftSourceSignature> &Callees = {}) const;
};

} // namespace neverd
#endif
