//===- RegistrationReadEffects.cpp - Private read permissions ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Withdraw fresh IAT facts when an ordinary read may dispatch caller code.
///
//===----------------------------------------------------------------------===//

#include "RegistrationReadEffects.h"

#include "RegistrationABIPrivate.h"
#include "RegistrationFrame.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

#include <cstdint>

namespace neverd::registration_abi {

bool hasClosedOrdinaryRegistrationRead(
    const BinaryImage &Image, const LowOp &Op,
    const registration_state::FrameTransfer &Transfer,
    uint32_t CallerArgumentBytes, size_t &Work) {
  using registration_state::FrameValue;
  using Thread = RegistrationSEHThreadAccess;
  if (Op.Opcode != NdOp::LOAD || Op.NumInputs != 1 || !Op.Output.Size ||
      Op.MemoryOrdering != NdMemoryOrdering::None || !chargeCalleeWork(Work, 1))
    return false;
  const auto Address = Transfer.read(Op.Inputs[0]);
  if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS)
    // The exact chain word and modeled thread fields belong to the current
    // Win32 TEB. Other segmented addresses retain no image-disjoint premise.
    return Op.Output.Size == Thread::WordBytes &&
           Thread::isAddressWidth(Op.Inputs[0].Size) && Address.Constant &&
           Address == FrameValue::constant(*Address.Constant) &&
           (*Address.Constant == 0 || *Address.Constant == Thread::SelfOffset ||
            *Address.Constant == Thread::StackBaseOffset);
  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto SP = Transfer.read(NdVar::reg(x86reg::RSP, sizeof(uint32_t)));
  if (Address.Offset && Address == FrameValue::frame(*Address.Offset) &&
      SP.Offset && SP == FrameValue::frame(*SP.Offset))
    // Uniform current near returns can additionally declare a callee-popped
    // argument extent. A maximum pop over unrelated returns cannot supply it.
    return *Address.Offset >= *SP.Offset &&
           int64_t(*Address.Offset) + Op.Output.Size <=
               int64_t(sizeof(uint32_t)) + CallerArgumentBytes;
  if (Address.ThreadAddress)
    return Address == FrameValue::threadBlock(*Address.ThreadAddress) &&
           Op.Output.Size == Thread::WordBytes &&
           Thread::isAddressWidth(Op.Inputs[0].Size) &&
           Address.ThreadAddress->Offset == Thread::StackBaseOffset;
  const auto VA = Address.preferredAddress();
  return VA &&
         hasSEHCallbackImageMemory(Image, *VA, Op.Output.Size, false, Work);
}

} // namespace neverd::registration_abi
