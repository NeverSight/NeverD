//===- RegistrationSEHRegisterInputs.cpp - Observable finally inputs -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A full register exchange preserves the callback ABI, but copying an unused
/// incoming register must not add an argument to the parent's physical ABI.

#include "neverd/Limits.h"
#include "neverd/ir/med/X86/X86RegistrationCallback.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"
#include "neverd/ir/med/X86/X86RegistrationSEHLocalUnwind.h"
#include "neverd/ir/med/X86/X86RegistrationSEHRegisters.h"

#include "llvm/ADT/STLExtras.h"

#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace neverd::x86_registration {

void discardUnusedSEHCallInputs(MedFunc &Function) {
  const auto Owners = registrationSEHCallbackOwners(Function);
  if (!Owners || Owners->empty())
    return;
  const auto LocalUnwinds = getX86RegistrationSEHLocalUnwinds(Function);
  const auto Returning = getRegistrationSEHReturns(Function);
  using ValueKey = std::tuple<MedVar::VarKind, int, int>;
  using RegisterKey = std::pair<va_t, uint64_t>;
  const auto Key = [](const MedVar &Value) {
    return ValueKey{Value.Kind, Value.Id, Value.SSAVer};
  };
  std::set<ValueKey> Uses;
  std::set<va_t> OpaqueCallbacks;
  std::map<RegisterKey, std::vector<ValueKey>> Reads;
  std::map<RegisterKey, std::vector<MedOp *>> Writes;
  std::map<RegisterKey, std::vector<ValueKey>> CallerReads;
  std::map<RegisterKey, std::vector<MedVar>> CallbackWrites;
  size_t Work = 0;
  const auto Charge = [&] {
    return ++Work <= limits::kMaxRegistrationEHStateWork;
  };
  const auto Use = [&](const MedVar &Value) {
    if (!Value.isConst() && Value.Id >= 0 && Value.Size)
      Uses.insert(Key(Value));
  };
  std::map<ValueKey, std::vector<ValueKey>> Dependencies;
  const auto Depends = [&](const ValueKey &Read, const MedVar &Value) {
    if (!Value.isConst() && Value.Id >= 0 && Value.Size)
      Dependencies[Read].push_back(Key(Value));
  };
  for (auto &Block : Function.Blocks) {
    if (!Charge())
      return;
    const auto Owner = Owners->find(Block.Id);
    for (const auto &Phi : Block.Phis) {
      if (!Charge() || !Phi.Output.Size || Phi.Output.Id < 0 ||
          Phi.Output.SSAVer <= 0)
        return;
      if (Phi.ExceptionalEntry) {
        if (Phi.ExceptionalEntry->Size != Phi.Output.Size)
          return;
        Depends(Key(Phi.Output), *Phi.ExceptionalEntry);
      }
      for (const auto &[Predecessor, Value] : Phi.Args) {
        if (!Charge() || Value.Size != Phi.Output.Size)
          return;
        Depends(Key(Phi.Output), Value);
      }
    }
    for (auto &Op : Block.Ops) {
      if (!Charge() || Op.NumInputs > Op.Inputs.size())
        return;
      if (!isSEHRegisterIntrinsic(Op)) {
        // A physical near return also names EAX as its scalar result. When
        // the exact same value already travels through the checked register
        // exchange, that spelling is not a second observer of the register.
        unsigned ReturnedRegisters = 0;
        if (Returning && Owner != Owners->end() && Owner->second.second &&
            Returning->ReturnBlocks.count(Block.Id) &&
            Returning->ReturnBlocks.at(Block.Id) == Owner->second.first &&
            Op.Opcode == NdOp::RETURN && Op.NumInputs == 1 &&
            Op.Inputs[0].Size == 4 && !Op.Output.Size &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            Op.MemoryOrdering == NdMemoryOrdering::None)
          for (const auto &Write : Block.Ops) {
            if (!Charge())
              return;
            if (hasValidSEHRegisterShape(Write) &&
                Write.Inputs[1].ConstVal ==
                    unsigned(SEHRegisterRole::CallbackOutput) &&
                Write.Inputs[2].ConstVal == Owner->second.first &&
                Write.Inputs[3].ConstVal == Op.Addr &&
                Write.Inputs[4].ConstVal == x86reg::RAX &&
                Write.Inputs[5] == Op.Inputs[0] &&
                Write.Inputs[5].Size == Op.Inputs[0].Size)
              ++ReturnedRegisters;
          }
        if (ReturnedRegisters != 1)
          for (unsigned I = 0; I != Op.NumInputs; ++I)
            Use(Op.Inputs[I]);
        if (Returning && Op.Opcode == NdOp::CALL && Op.Output.Size) {
          const auto Call =
              Returning->ReturningCalls.find({Block.Id, Op.Addr, Op.OriginSeq});
          if (Call != Returning->ReturningCalls.end()) {
            if (Op.Output.Size != 4 || Op.Output.Id < 0 ||
                Op.Output.SSAVer <= 0)
              return;
            CallerReads[{Call->second, x86reg::RAX}].push_back(Key(Op.Output));
          }
        }
        // A call or another intrinsic can consume physical registers through
        // an implicit ABI which has not yet been recovered. Keep all inputs
        // of such a callback, even if no explicit SSA use names the register.
        bool CheckedLocal = false;
        if (LocalUnwinds)
          if (const auto Local = LocalUnwinds->find(Op.Addr);
              Local != LocalUnwinds->end())
            CheckedLocal = Local->second.Operation == &Op;
        if (!CheckedLocal && Owner != Owners->end() &&
            (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
             Op.Opcode == NdOp::INTRINSIC))
          OpaqueCallbacks.insert(Owner->second.first);
        continue;
      }
      if (!hasValidSEHRegisterShape(Op))
        return;
      const RegisterKey Register{Op.Inputs[2].ConstVal, Op.Inputs[4].ConstVal};
      const auto Role = static_cast<SEHRegisterRole>(Op.Inputs[1].ConstVal);
      if (Role == SEHRegisterRole::CallbackInput) {
        if (Owner == Owners->end() || !Owner->second.second ||
            Owner->second.first != Register.first ||
            Block.StartAddr != Op.Addr || Op.Output.SSAVer <= 0)
          return;
        Reads[Register].push_back(Key(Op.Output));
      } else if (Role == SEHRegisterRole::CallInput)
        Writes[Register].push_back(&Op);
      else if (Role == SEHRegisterRole::CallbackOutput) {
        if (!Returning || Owner == Owners->end() || !Owner->second.second ||
            Owner->second.first != Register.first ||
            !Returning->ReturnBlocks.count(Block.Id) ||
            Returning->ReturnBlocks.at(Block.Id) != Register.first ||
            Block.Ops.back().Opcode != NdOp::RETURN ||
            Block.Ops.back().Addr != Op.Addr)
          return;
        CallbackWrites[Register].push_back(Op.Inputs[5]);
      } else {
        if (!Returning || Op.Output.SSAVer <= 0)
          return;
        bool Bound = false;
        for (const auto &[Call, Entry] : Returning->ReturningCalls) {
          if (!Charge())
            return;
          if (std::get<0>(Call) == Block.Id && std::get<1>(Call) == Op.Addr &&
              Entry == Register.first)
            Bound = true;
        }
        if (!Bound)
          return;
        CallerReads[Register].push_back(Key(Op.Output));
      }
    }
  }
  for (const auto &Clobber : Function.CallClobbers) {
    if (!Charge())
      return;
    if (Clobber.PreservedPrefixSize)
      Use(Clobber.PreservedInput);
  }
  for (const auto &Call : Function.CallInfos)
    for (const auto &Argument : Call.Args) {
      if (!Charge())
        return;
      Use(Argument);
    }
  // A register copied unchanged through a finally is observable only when
  // its caller consumes the returned value. Follow the checked transport in
  // both directions; a cycle of unused exchanges creates no physical input.
  for (const auto &[Register, Inputs] : Reads) {
    for (const auto &Read : Inputs) {
      if (OpaqueCallbacks.count(Register.first))
        Uses.insert(Read);
      for (const auto *Write : Writes[Register]) {
        if (!Charge())
          return;
        Depends(Read, Write->Inputs[5]);
      }
    }
  }
  for (const auto &[Register, Outputs] : CallbackWrites) {
    const auto Caller = CallerReads.find(Register);
    if (Caller == CallerReads.end()) {
      for (const auto &Value : Outputs)
        Use(Value);
      continue;
    }
    for (const auto &Read : Caller->second)
      for (const auto &Value : Outputs) {
        if (!Charge())
          return;
        Depends(Read, Value);
      }
  }
  for (const auto &[Register, Outputs] : CallerReads)
    if (!CallbackWrites.count(Register))
      return;
  std::deque<ValueKey> Pending(Uses.begin(), Uses.end());
  while (!Pending.empty()) {
    const auto Read = Pending.front();
    Pending.pop_front();
    if (!Charge())
      return;
    const auto Found = Dependencies.find(Read);
    if (Found == Dependencies.end())
      continue;
    for (const auto &Value : Found->second) {
      if (!Charge())
        return;
      if (Uses.insert(Value).second)
        Pending.push_back(Value);
    }
  }
  // No mutation precedes the complete scan. Unknown/malformed transport and
  // budget exhaustion preserve the original conservative parameter recovery.
  for (const auto &[Register, Operations] : Writes) {
    const auto Found = Reads.find(Register);
    if (Found == Reads.end() || OpaqueCallbacks.count(Register.first) ||
        llvm::any_of(Found->second,
                     [&](const auto &Read) { return Uses.count(Read); }))
      continue;
    for (auto *Op : Operations)
      Op->Inputs[5] = MedVar::makeUnspecified(4, Arch::X86);
  }
}

} // namespace neverd::x86_registration
