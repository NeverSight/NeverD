//===- RegistrationStateObject.cpp - PE32 exception object lifetimes -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind each runtime exception pointer to its still-live catch invocation.
//===----------------------------------------------------------------------===//
#include "RegistrationStateSolver.h"

namespace neverd::registration_state {
bool RegistrationStateSolver::runtimeObjectIsLive(
    const Domain &State, std::pair<uint32_t, uint32_t> Identity) {
  if (State.Parent || State.OtherCallback || State.CxxCatchStacks.empty())
    return false;
  for (const auto &Stack : State.CxxCatchStacks) {
    if (!charge(Stack.size() + 1) ||
        !llvm::any_of(Stack, [&](const auto &Context) {
          return Context.TryIndex == Identity.first &&
                 Context.CatchIndex == Identity.second;
        }))
      return false;
  }
  return true;
}

bool RegistrationStateSolver::recordRuntimeMemory(
    size_t I, Domain &After, const LowOp &Op, const FrameTransfer &Transfer,
    const FrameTransfer &RuntimeTransfer) {
  const auto Memory = lowMemoryOperands(Op);
  if (!CheckRuntimeObjects || !Memory.Address)
    return false;
  const LowBlock &Block = Function.Blocks[I];
  bool RuntimeMemory = false;
  const auto RuntimeAddress = RuntimeTransfer.read(*Memory.Address);
  if (RuntimeAddress.MayBeFrame) {
    const auto Identity = std::make_pair(Op.Addr, Op.Seq);
    const auto Boundary = Boundaries.find(Op.Addr);
    const auto Object = RuntimeAddress.ExceptionObject
                            ? CatchObjects.find(*RuntimeAddress.ExceptionObject)
                            : CatchObjects.end();
    bool Valid =
        Memory.Complete &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
        !After.Unknown && !Facts[I].Invalid && Object != CatchObjects.end() &&
        runtimeObjectIsLive(After, Object->first) && RuntimeAddress.Offset &&
        *RuntimeAddress.Offset >= 0 && Op.Seq >= 0 &&
        Boundary != Boundaries.end() && Boundary->second.first == Block.Id &&
        uint64_t(*RuntimeAddress.Offset) + Memory.AccessSize <=
            Object->second.ObjectSize;
    if (Valid && Op.Opcode == NdOp::STORE)
      Valid = !Transfer.read(*Memory.StoredValue).MayBeFrame &&
              !RuntimeTransfer.read(*Memory.StoredValue).MayBeFrame;
    RegistrationRuntimeObjectAccess Access;
    if (Valid) {
      const auto &Contract = Object->second;
      Access = {Op.Addr,
                Op.Seq,
                Contract.TryIndex,
                Contract.CatchIndex,
                *RuntimeAddress.Offset,
                Memory.AccessSize,
                Op.Opcode == NdOp::STORE};
      auto [It, New] = RuntimeAccesses.emplace(Identity, Access);
      Valid = (New || It->second == Access) &&
              !InvalidRuntimeAccesses.count(Identity);
    }
    if (!Valid) {
      InvalidRuntimeAccesses.insert(Identity);
      RuntimeAccesses.erase(Identity);
      CompleteRuntimeObjects = CompleteImageReads = false;
    }
    RuntimeMemory = Valid;
  }
  if (Op.Opcode == NdOp::STORE && Memory.StoredValue) {
    const auto Stored = RuntimeTransfer.read(*Memory.StoredValue);
    const auto PrivateAddress = Transfer.read(*Memory.Address);
    if (PrivateAddress.Offset && !RuntimeAddress.MayBeFrame) {
      if (!charge(After.RuntimeObject.cellCount() +
                  (Memory.AccessSize + 3) / 4 + 1))
        return false;
      After.RuntimeObject.store(*PrivateAddress.Offset, Memory.AccessSize,
                                Stored);
    } else if (Stored.MayBeFrame) {
      CompleteRuntimeObjects = false;
    }
  }
  return RuntimeMemory;
}

} // namespace neverd::registration_state
