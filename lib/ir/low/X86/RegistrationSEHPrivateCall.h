//===- RegistrationSEHPrivateCall.h - SEH private call effects -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONSEHPRIVATECALL_H
#define NEVERD_IR_LOW_X86_REGISTRATIONSEHPRIVATECALL_H

#include "neverd/Common.h"

#include <cstddef>
#include <map>
#include <optional>

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct LowOp;

namespace registration_state {

/// One immutable source-analysis invocation. A bounded callee graph may
/// preserve the caller's private frame without granting a return value or a
/// native call ABI. Native publication still replays the whole caller closure,
/// including image effects and caller-PC readback.
class SEHPrivateCallProof {
public:
  SEHPrivateCallProof(const BinaryImage &Image, const LowFunc &Function)
      : Image(Image), Function(Function) {}

  bool preservesFrame(const LowOp &Op, size_t &Work);
  /// Check explicit returning ESP with all nested calls opaque. This answer
  /// neither preserves the caller frame nor authenticates an import binding.
  std::optional<uint32_t> opaqueStackPop(const LowOp &Op, size_t &Work);

private:
  bool proveCallee(va_t Target, size_t &Work);

  const BinaryImage &Image;
  const LowFunc &Function;
  std::map<va_t, bool> Callees;
  std::map<const LowOp *, bool> Occurrences;
  std::map<va_t, std::optional<uint32_t>> StackTargets;
  std::map<const LowOp *, std::optional<uint32_t>> StackOccurrences;
};

} // namespace registration_state
} // namespace neverd

#endif
