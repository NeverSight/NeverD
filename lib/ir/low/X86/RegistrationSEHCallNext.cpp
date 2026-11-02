//===- RegistrationSEHCallNext.cpp - Adjacent physical finally calls -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Recovers an adjacent finally invocation after generic get-PC lifting.

#include "RegistrationSEHControl.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

namespace neverd::registration_state {

void restoreAdjacentSEHFinallyCall(const BinaryImage &Image,
                                   const ExceptionFunction &Exception,
                                   va_t Address, uint16_t Size,
                                   std::vector<LowOp> &Ops) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || !Exception.Registration ||
      Exception.Cxx ||
      Exception.ParseStatus != ExceptionParseStatus::Complete ||
      (Exception.Encoding != ExceptionEncoding::X86ScopeTableEH3 &&
       Exception.Encoding != ExceptionEncoding::X86ScopeTableEH4) ||
      Size != 5 || Address > UINT32_MAX - Size || Ops.size() != 2 ||
      !Exception.ownsCode({Address, Address + Size + 1}))
    return;
  const auto Stack = NdVar::reg(x86reg::RSP, 4);
  const auto &Adjust = Ops[0];
  const auto &Push = Ops[1];
  if (Adjust.Opcode != NdOp::INT_SUB || Adjust.Output != Stack ||
      Adjust.NumInputs != 2 || Adjust.Inputs[0] != Stack ||
      !Adjust.Inputs[1].isConst() || Adjust.Inputs[1].Size != 4 ||
      Adjust.Inputs[1].Offset != 4 || Push.Opcode != NdOp::STORE ||
      Push.Output.Size || Push.NumInputs != 2 || Push.Inputs[0] != Stack ||
      !Push.Inputs[1].isConst() || Push.Inputs[1].Size != 4 ||
      Push.Inputs[1].Offset != Address + Size || Adjust.Addr != Address ||
      Push.Addr != Address || Adjust.Seq != 0 || Push.Seq != 1 ||
      Adjust.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Push.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Adjust.MemoryOrdering != NdMemoryOrdering::None ||
      Push.MemoryOrdering != NdMemoryOrdering::None)
    return;
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Address;
  Call.Seq = 0;
  Call.Output = NdVar::reg(x86reg::RAX, 4);
  Call.addInput(Push.Inputs[1]);
  if (!isSEHFinallyCallInstruction(Image, *Exception.Registration, Address,
                                   Size, {Call}))
    return;
  // Ordinary get-PC sequences retain their architectural push. Only the
  // source-owned finally protocol can hide this PC behind a callable ABI;
  // its separate frame proof must still reject observing that return slot.
  Ops = {Call};
}

} // namespace neverd::registration_state
