//===- MIPS16EntryAdapter.h - Proven classic FP entry transport -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_MIPS_MIPS16ENTRYADAPTER_H
#define NEVERD_LIB_PIPELINE_MIPS_MIPS16ENTRYADAPTER_H

#include "neverd/ir/FloatingCallArguments.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd {
class DebugContext;
class ExecutableCodeOwnerIndex;
struct BinaryImage;
struct MedFunc;

namespace pipeline_detail {

/// A classic entry whose complete body transports the declared parameters to
/// one MIPS16 entry. This is not a name-based alias or a function-pointer
/// proof.
struct MIPS16EntryAdapter {
  va_t Body = InvalidVA;
  FloatingCallArguments EntryArguments;
  FloatingCallArguments BodyArguments;
  bool UserDeclared = false;
  std::string BodyName;
};

std::optional<MIPS16EntryAdapter>
proveMIPS16EntryAdapter(const BinaryImage &Image, va_t Entry,
                        const DebugContext &Debug,
                        const ExecutableCodeOwnerIndex *Owners = nullptr);

/// Source routes only, after calls have their complete logical operands.
/// Preserve LowIR machine targets and retarget only the bound source call.
void projectMIPS16EntryCalls(const BinaryImage &Image,
                             std::vector<MedFunc> &Functions,
                             const DebugContext *Debug);

} // namespace pipeline_detail
} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_MIPS_MIPS16ENTRYADAPTER_H
