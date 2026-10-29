//===- RegistrationMemoryPermissions.cpp - Private accesses --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Withdraw fresh IAT facts when an ordinary access may dispatch caller code.
///
//===----------------------------------------------------------------------===//

#include "RegistrationMemoryPermissions.h"

#include "RegistrationABIPrivate.h"
#include "RegistrationFrame.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

#include <cstdint>

namespace neverd::registration_abi {

bool hasClosedOrdinaryRegistrationMemoryAccess(
    const BinaryImage &Image, const LowOp &Op,
    const registration_state::FrameTransfer &Transfer,
    uint32_t CallerArgumentBytes, size_t &Work) {
  using registration_state::FrameValue;
  using Thread = RegistrationSEHThreadAccess;
  const bool Write = Op.Opcode == NdOp::STORE;
  if ((Write ? Op.NumInputs != 2 || Op.Output.Size
             : Op.Opcode != NdOp::LOAD || Op.NumInputs != 1) ||
      Op.MemoryOrdering != NdMemoryOrdering::None || !chargeCalleeWork(Work, 1))
    return false;
  const auto Memory = lowMemoryOperands(Op);
  if (!Memory.Complete || !Memory.Address || bool(Memory.StoredValue) != Write)
    return false;
  const auto Width = Memory.AccessSize;
  const auto Address = Transfer.read(*Memory.Address);
  if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS)
    // The exact chain word and modeled thread fields belong to the current
    // Win32 TEB. Other segmented addresses retain no image-disjoint premise.
    return Width == Thread::WordBytes &&
           Thread::isAddressWidth(Memory.Address->Size) && Address.Constant &&
           Address == FrameValue::constant(*Address.Constant) &&
           (*Address.Constant == 0 ||
            (!Write && (*Address.Constant == Thread::SelfOffset ||
                        *Address.Constant == Thread::StackBaseOffset)));
  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto SP = Transfer.read(NdVar::reg(x86reg::RSP, sizeof(uint32_t)));
  if (Address.Offset && Address == FrameValue::frame(*Address.Offset) &&
      SP.Offset && SP == FrameValue::frame(*SP.Offset))
    // Uniform current near returns can additionally declare a callee-popped
    // argument extent. A maximum pop over unrelated returns cannot supply it.
    return *Address.Offset >= *SP.Offset &&
           int64_t(*Address.Offset) + Width <=
               int64_t(sizeof(uint32_t)) + CallerArgumentBytes;
  if (Address.ThreadAddress)
    return !Write &&
           Address == FrameValue::threadBlock(*Address.ThreadAddress) &&
           Width == Thread::WordBytes &&
           Thread::isAddressWidth(Memory.Address->Size) &&
           Address.ThreadAddress->Offset == Thread::StackBaseOffset;
  const auto VA = Address.preferredAddress();
  return VA && hasSEHCallbackImageMemory(Image, *VA, Width, Write, Work);
}

bool preservesOrdinaryRegistrationBindings(
    const BinaryImage &Image, const LowOp &Op,
    const registration_state::FrameTransfer &Transfer,
    uint32_t CallerArgumentBytes, size_t &Work) {
  using registration_state::FrameValue;
  if (!chargeCalleeWork(Work, 1) || Op.MemoryOrdering != NdMemoryOrdering::None)
    return false;
  if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) {
    const auto Memory = lowMemoryOperands(Op);
    if (!Memory.Complete || !Memory.Address ||
        Memory.AccessSize > sizeof(uint32_t))
      return false;
    if (Op.Opcode == NdOp::STORE) {
      const auto Address = Transfer.read(*Memory.Address);
      // This summary protects all caller bindings, not just the dependencies
      // used by this body. It also cannot let a changed return PC dispatch
      // unexamined code. Keep writes strictly below the original entry SP.
      if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          !Address.Offset || Address != FrameValue::frame(*Address.Offset) ||
          int64_t(*Address.Offset) + Memory.AccessSize > 0)
        return false;
    }
    return hasClosedOrdinaryRegistrationMemoryAccess(Image, Op, Transfer,
                                                     CallerArgumentBytes, Work);
  }
  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_MULT:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::RETURN:
  case NdOp::INT_NEG2:
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
  case NdOp::SELECT:
  case NdOp::NOP:
    return true;
  default:
    return false;
  }
}

} // namespace neverd::registration_abi
