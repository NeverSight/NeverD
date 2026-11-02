//===- RegistrationEpilogue.cpp - Physical C++ exit projection ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationEpilogue.h"

#include "RegistrationHelperExpansion.h"

#include "neverd/decode/Decoder.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/ir/low/RegistrationSharedCall.h"
#include "neverd/lift/LiftCommon.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd::x86_registration {

void projectCxxEpilogues(const BinaryImage &Image, const LowFunc &Source,
                         LowFunc &Physical) {
  if (!Source.ExceptionMetadata || !Source.RegistrationStates ||
      !Source.RegistrationStates->ChainOperationsComplete ||
      !Source.RegistrationStates->CxxPrologue ||
      Physical.Blocks.size() != Source.Blocks.size())
    return;
  auto TempId = nextRegistrationHelperTemp(Physical);
  if (!TempId)
    return;
  for (size_t I = 0; I != Source.Blocks.size(); ++I) {
    const auto Call = getCheckedX86RegistrationCxxEpilogueCall(
        Image, Source, Source.Blocks[I]);
    // A source cookie receipt cannot replace its checker and failure path.
    if (!Call || Call->CookieCheck)
      continue;
    auto Exit =
        expandRegistrationHelper(Image, Source.Entry, Call->Caller.Begin,
                                 Call->Caller.End, Call->Helper, *TempId);
    if (!Exit)
      continue;
    auto &Block = Physical.Blocks[I];
    Block.Ops.pop_back();
    Block.Ops.insert(Block.Ops.end(), Exit->first.begin(), Exit->first.end());
    Block.InstructionBoundaries.back().OpCount = Exit->first.size();
    TempId = Exit->second;
  }
}

} // namespace neverd::x86_registration
