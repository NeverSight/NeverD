//===- MedLLVMRegistrationFrame.cpp - x86 runtime root lowering -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/X86RegistrationFrame.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86RegistrationFrame.h"

#include <stdexcept>

namespace neverd {

llvm::Value *emitX86RegistrationRoot(const MedFunc &Func, const MedOp &Op,
                                     llvm::Value *EntrySP,
                                     llvm::IRBuilder<> &Builder) {
  if (!hasValidRegistrationRootShape(Op) || !EntrySP ||
      !EntrySP->getType()->isIntegerTy(32))
    throw std::invalid_argument("invalid PE32 registration runtime root");
  if (Op.RegistrationRoot == MedOp::RegistrationRootKind::CallbackStackPointer)
    return EntrySP;
  const auto Coordinate = registrationRootFrameCoordinate(Func, Op);
  if (!Coordinate)
    throw std::invalid_argument("unproved PE32 registration frame coordinate");
  llvm::Value *Address = Builder.CreateAdd(
      EntrySP, Builder.getInt32(uint32_t(Coordinate->EntryOffset)),
      "registration.source.frame");
  if (Coordinate->Alignment != 1)
    Address =
        Builder.CreateAnd(Address, Builder.getInt32(-Coordinate->Alignment),
                          "registration.aligned.frame");
  if (Coordinate->AlignedOffset)
    Address = Builder.CreateAdd(
        Address, Builder.getInt32(uint32_t(Coordinate->AlignedOffset)),
        "registration.runtime.frame");
  return Address;
}

} // namespace neverd
