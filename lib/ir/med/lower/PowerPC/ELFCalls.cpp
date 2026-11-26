//===- ELFCalls.cpp - PPC64 descriptor and TOC lowering ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recover native PPC64 ELF calls and TOC-relative operands
/// from their reaching machine definitions.
///
//===----------------------------------------------------------------------===//

#include "ELFCalls.h"

#include "../../../../safety/StackSlotFlow.h"
#include "../../pass/MedEntryStackOffsets.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/IndirectCallSlot.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/IntegerRegisters.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/ELF/PPC64FunctionDescriptor.h"
#include "neverd/loader/ELF/PowerPCABI.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace neverd {
namespace {
using namespace ppc_elf;

MedVar stripCopies(MedVar V, MedDefinitionIndex &Defs) {
  for (int I = 0; I < limits::kCallTargetSlotDepth && !V.isConst(); ++I) {
    const auto *D = Defs.find(V);
    if (!D || D->Opcode != NdOp::COPY || D->NumInputs != 1 ||
        D->Inputs[0].Size != WordSize || D->Inputs[0] == V)
      break;
    V = D->Inputs[0];
  }
  return V;
}

// A high-adjusted TOC displacement can first produce an address outside any
// section. Bind the completed load/store address only after the low half.
std::optional<va_t> tocAddress(MedVar V, MedDefinitionIndex &Defs,
                               std::optional<va_t> TOC,
                               std::optional<va_t> Global = std::nullopt) {
  uint64_t Displacement = 0;
  for (int I = 0; I < limits::kCallTargetSlotDepth && V.Size == WordSize; ++I) {
    if (V.isConst()) {
      if (((V.Provenance == ConstantAddressProvenance::AddressFragment ||
            V.Provenance == ConstantAddressProvenance::DataAddress) &&
           (TOC == V.ConstVal || Global == V.ConstVal)) ||
          (V.Provenance == ConstantAddressProvenance::CodeAddress &&
           Global == V.ConstVal))
        return V.ConstVal + Displacement;
      break;
    }
    const auto *D = Defs.find(V);
    if (!D)
      break;
    if (D->Opcode == NdOp::COPY && D->NumInputs == 1) {
      V = D->Inputs[0];
      continue;
    }
    if ((D->Opcode != NdOp::INT_ADD && D->Opcode != NdOp::INT_SUB) ||
        D->NumInputs != 2 || !D->Inputs[1].isConst() ||
        D->Inputs[1].Provenance != ConstantAddressProvenance::Scalar)
      break;
    Displacement += D->Opcode == NdOp::INT_ADD ? D->Inputs[1].ConstVal
                                               : -D->Inputs[1].ConstVal;
    V = D->Inputs[0];
  }
  return std::nullopt;
}

// Only loads still current at the branch can be replaced by the native ABI's
// descriptor call. A store or another call can change the descriptor.
std::optional<MedVar>
fieldBase(MedVar V, unsigned Offset, MedDefinitionIndex &Defs,
          const llvm::SmallPtrSetImpl<const MedOp *> &Loads) {
  const auto *Load = Defs.find(stripCopies(V, Defs));
  if (!Load || !Loads.contains(Load))
    return std::nullopt;
  MedVar EA = stripCopies(Load->Inputs[0], Defs);
  if (Offset == EntryOffset)
    return EA;
  const auto *Add = Defs.find(EA);
  if (!Add || Add->Opcode != NdOp::INT_ADD || Add->NumInputs != 2 ||
      !Add->Inputs[1].isConst() ||
      Add->Inputs[1].Provenance != ConstantAddressProvenance::Scalar ||
      Add->Inputs[1].ConstVal != Offset)
    return std::nullopt;
  return stripCopies(Add->Inputs[0], Defs);
}

// A linked data word can numerically equal an .opd address without being a
// pointer. Once a descriptor call proves the use, standalone code must either
// rebuild the word's initializer or report the missing relocation.
void checkDescriptorInitializer(const MedVar &Base, MedDefinitionIndex &Defs,
                                const BinaryImage &Image,
                                std::optional<va_t> EntryTOC) {
  const auto *Load = Defs.find(Base);
  if (!Load || Load->Opcode != NdOp::LOAD || Load->Output.Size != WordSize)
    return;
  const auto Address = stripCopies(Load->Inputs[0], Defs);
  std::optional<va_t> Slot;
  if (Address.isConst() && isDataAddressProvenance(Address.Provenance))
    Slot = Address.ConstVal;
  else if (EntryTOC)
    Slot = tocAddress(Address, Defs, *EntryTOC);
  if (!Slot || Image.CodePtrRelocSlots.count(*Slot))
    return;
  if (const auto *Bytes = Image.readVA(*Slot, WordSize))
    if (Image.PPC64Descriptors.contains(
            readPtr(Bytes, true, archLittleEndian(Image.Arch))))
      throw LowToMedConversionError("PPC64 ELFv1 function-pointer initializer "
                                    "has no supported relocation");
}
// A signed high adjustment can lie outside the image until the following add
// or memory access contributes the low half. Keep the original definitions
// available until every use has been inspected.
void bindTOCAddresses(MedFunc &Function, MedDefinitionIndex &Defs,
                      const BinaryImage &Image, va_t TOC) {
  llvm::SmallVector<std::pair<MedOp *, MedVar>> Pointers;
  for (auto &Block : Function.Blocks) {
    for (auto &Op : Block.Ops) {
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        if (auto VA = tocAddress(Op.Inputs[0], Defs, TOC);
            VA && Image.isDataAddress(*VA))
          Op.Inputs[0] = MedVar::makeConst(
              *VA, WordSize, ConstantAddressProvenance::DataAddress);
        continue;
      }
      if (Op.Output.Size != WordSize ||
          (Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB))
        continue;
      const auto VA = tocAddress(Op.Output, Defs, TOC);
      if (!VA)
        continue;
      const auto It = Image.PPC64Descriptors.find(*VA);
      if (It == Image.PPC64Descriptors.end()) {
        if (Image.isDataAddress(*VA))
          Pointers.emplace_back(
              &Op, MedVar::makeConst(*VA, WordSize,
                                     ConstantAddressProvenance::DataAddress));
        continue;
      }
      const auto Entry = nativePPC64DescriptorEntry(Image, *VA);
      if (!Entry)
        throw LowToMedConversionError(
            "PPC64 ELFv1 address requires a distinct function descriptor");
      Pointers.emplace_back(
          &Op, MedVar::makeConst(*Entry, WordSize,
                                 ConstantAddressProvenance::CodeAddress));
    }
  }
  for (auto &[Op, Pointer] : Pointers) {
    Op->Opcode = NdOp::COPY;
    Op->NumInputs = 1;
    Op->Inputs[0] = Pointer;
  }
}
// R_PPC64_TOCSAVE lets the linker put the save in the caller's prologue.
// Match its actual reaching stack word, including across repeated calls that
// use the same linkage area. Other calls and aliasing stores kill the proof.
void recoverCallerTOCRestores(MedFunc &Function, const BinaryImage &Image,
                              const PPC64LinkageCalls &Calls) {
  if (Calls.CallerRestores.empty())
    return;
  MedEntryStackOffsets Offsets(Function, Image.Arch, BinaryFormat::ELF);
  auto Offset = [&](const MedVar &V) { return Offsets.query(V); };
  llvm::DenseMap<uint32_t, int64_t> CallSlots;
  struct Reload {
    MedBlock *Block;
    size_t Index;
    int64_t Slot;
  };
  llvm::SmallVector<Reload> Reloads;
  for (auto &Block : Function.Blocks)
    for (auto [I, Op] : llvm::enumerate(Block.Ops)) {
      if (Op.Opcode != NdOp::LOAD || Op.Output.Size != WordSize)
        continue;
      auto Call = Calls.CallerRestores.find(Op.Addr);
      if (Call == Calls.CallerRestores.end())
        continue;
      auto At = Offset(Op.Inputs[0]);
      if (!At)
        throw LowToMedConversionError(
            "PPC64 TOC restore has no proven stack slot");
      CallSlots.try_emplace(Call->second, *At);
      Reloads.push_back({&Block, I, *At});
    }
  auto Preserves = [&](const MedOp &Op, int64_t Slot, uint16_t Bytes) {
    auto It = CallSlots.find(Op.CallSiteId);
    return It != CallSlots.end() && It->second == Slot && Bytes == WordSize;
  };
  MedDefinitionIndex Defs(Function);
  auto MayBeFrame = [&](MedVar V) {
    V = stripCopies(V, Defs);
    // The prologue can save LR after r2. Its incoming return address points
    // to the caller's code and cannot expose the linkage area to the callee.
    return !V.isConst() &&
           !(V.Kind == MedVar::Reg && V.RegOff == integerreg::ppc::LR &&
             safety::detail::isAuthenticatedEntryRegisterLiveIn(Function, V));
  };
  llvm::SmallVector<std::pair<MedOp *, MedVar>> Copies;
  for (const auto &[Block, I, Slot] : Reloads) {
    const auto Reaching = safety::detail::reachingStackValues(
        Function, Block->Id, I, Slot, WordSize, Offset, MayBeFrame,
        [](const MedVar &) { return false; }, true, true, Preserves);
    if (!Reaching.Complete || Reaching.Values.size() != 1)
      throw LowToMedConversionError(
          "PPC64 TOC restore has no unique reaching save");
    Copies.emplace_back(&Block->Ops[I], Reaching.Values.front());
  }
  for (auto &[Op, V] : Copies) {
    Op->Opcode = NdOp::COPY;
    Op->Inputs[0] = V;
  }
}
} // namespace

void requirePPCConditionalReturnLinks(const LowFunc &Function) {
  // Only predecessors of a guarded LR transfer matter. An LR write on the
  // other side of an early return does not change the link at that return.
  llvm::SmallVector<int> Work;
  llvm::SmallDenseSet<int> Visited;
  auto Check = [&](const LowBlock &Block, llvm::ArrayRef<LowOp> Before) {
    if (!Block.ExceptionalPreds.empty() ||
        (Block.StartAddr != Function.Entry &&
         (Block.Preds.empty() ||
          Function.ModuleAnalysisRoots.count(Block.StartAddr))))
      throw LowToMedConversionError(
          "PPC conditional return has an unknown incoming link");
    for (const auto &Op : Before)
      if ((Op.Output.isReg() && Op.Output.Offset == integerreg::ppc::LR) ||
          Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        throw LowToMedConversionError(
            "PPC conditional return does not have a proven incoming LR");
    for (int Pred : Block.Preds)
      if (Visited.insert(Pred).second)
        Work.push_back(Pred);
  };
  for (const auto &Block : Function.Blocks) {
    if (Block.InstructionBoundaries.empty())
      continue;
    const auto &Last = Block.InstructionBoundaries.back();
    if (Last.Control == LowInstructionControl::ConditionalReturn)
      Check(Block, llvm::ArrayRef(Block.Ops).take_front(Last.FirstOp));
  }
  while (!Work.empty()) {
    const auto &Block = Function.Blocks[Work.pop_back_val()];
    Check(Block, Block.Ops);
  }
}

PPC64LinkageCalls preparePPC64LinkageCalls(MedFunc &Function,
                                           const BinaryImage &Image, MedVar TOC,
                                           MedVar SP, MedVar R12,
                                           int &NextVarId) {
  using namespace ppc_elf;
  PPC64LinkageCalls Calls;
  const bool V2 = isPowerPC64ELFv2(Image.Arch);
  const auto Entry = Image.PPC64EntryContexts.find(Function.Entry);
  std::map<va_t, std::pair<int, MedVar>> Restores;
  auto Fresh = [&]() {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = NextVarId++;
    V.Size = WordSize;
    return V;
  };
  for (auto &Block : Function.Blocks) {
    // A block that executes the entire checked global prologue establishes
    // its TOC. A branch into the middle keeps the original arithmetic.
    if (V2 && Entry != Image.PPC64EntryContexts.end() && Entry->second.TOC &&
        Block.StartAddr == Function.Entry && Block.Preds.empty() &&
        Block.ExceptionalPreds.empty()) {
      const auto Local = Function.Entry + Entry->second.LocalEntryOffset;
      if (Block.EndAddr >= Local) {
        MedOp *Last = nullptr;
        for (auto &Op : Block.Ops) {
          if (Op.Addr >= Local)
            break;
          if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == TOC.RegOff &&
              Op.Output.Size == WordSize)
            Last = &Op;
        }
        if (Last) {
          Last->Opcode = NdOp::COPY;
          Last->NumInputs = 1;
          Last->Inputs[0] =
              MedVar::makeConst(*Entry->second.TOC, WordSize,
                                ConstantAddressProvenance::AddressFragment);
        }
      }
    }
    std::vector<MedOp> Ops;
    size_t First = 0;
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      auto &Op = Block.Ops[I];
      const bool Indirect = V2 && Op.Opcode == NdOp::INDIR_CALL;
      if (!Op.NumInputs ||
          (!Indirect && (Op.Opcode != NdOp::CALL || !Op.Inputs[0].isConst())))
        continue;
      va_t Target = Indirect ? InvalidVA : Op.Inputs[0].ConstVal;
      const auto Stub = Image.PPC64LinkageStubs.find(Target);
      const va_t Global = Image.ppc64GlobalEntry(Target);
      auto Callee = Image.PPC64EntryContexts.find(Global);
      const bool Saves =
          Stub != Image.PPC64LinkageStubs.end() && Stub->second.SavesTOC;
      const va_t Resume = Op.Addr + InstructionAlignment;
      const auto *Next = Image.readVA(Resume, InstructionAlignment);
      const bool RestoresTOC =
          !Op.IsTailCall && Next &&
          readTargetEndian<uint32_t>(Next, archLittleEndian(Image.Arch)) ==
              (V2 ? ELFv2RestoreTOC : RestoreTOC);
      va_t Expected = InvalidVA;
      bool NeedsR12 = Indirect;
      if (!Indirect && Stub != Image.PPC64LinkageStubs.end()) {
        if (Stub->second.Target == InvalidVA)
          throw LowToMedConversionError(
              "PPC64 linkage has no unique call target or TOC");
        Expected = Stub->second.IncomingTOC;
        if (V2)
          Calls.CallTargets.try_emplace(Op.CallSiteId, Stub->second.Target);
        else
          Op.Inputs[0] =
              MedVar::makeConst(Stub->second.Target, WordSize,
                                ConstantAddressProvenance::CodeAddress);
      } else if (V2 && Callee != Image.PPC64EntryContexts.end()) {
        if (Global != Target) {
          if (!Callee->second.TOC)
            throw LowToMedConversionError(
                "PPC64 ELFv2 local entry has no proven TOC");
          Expected = *Callee->second.TOC;
          Calls.CallTargets.try_emplace(Op.CallSiteId, Global);
        } else if (Callee->second.LocalEntryOffset) {
          NeedsR12 = true;
          Expected = Global;
        }
      } else if (Callee != Image.PPC64EntryContexts.end() &&
                 Callee->second.TOC) {
        Expected = *Callee->second.TOC;
      } else if (!Indirect) {
        if (RestoresTOC)
          throw LowToMedConversionError(
              "PPC64 TOC restore has no proven linkage call");
        continue;
      }

      if (!Indirect && !Saves && Expected == InvalidVA) {
        if (RestoresTOC)
          Calls.CallerRestores.try_emplace(Resume, Op.CallSiteId);
        continue;
      }

      // Move each original operation once while retaining the call's
      // occurrence and all of its ABI operands.
      if (Ops.empty())
        Ops.reserve(Block.Ops.size());
      Ops.insert(Ops.end(), std::make_move_iterator(Block.Ops.begin() + First),
                 std::make_move_iterator(Block.Ops.begin() + I));
      First = I;
      MedOp Snapshot;
      Snapshot.Opcode = NdOp::COPY;
      Snapshot.Addr = Op.Addr;
      Snapshot.Output = Fresh();
      Snapshot.addInput(NeedsR12 ? R12 : TOC);
      const MedVar Saved = Snapshot.Output;
      if (Indirect)
        Calls.IndirectEntrySnapshots.insert(Saved.Id);
      if (Expected != InvalidVA)
        (NeedsR12 ? Calls.RequiredGlobalEntries : Calls.RequiredTOCs)
            .try_emplace(Saved.Id, Expected);
      Ops.push_back(std::move(Snapshot));
      if (Saves) {
        MedOp Address;
        Address.Opcode = NdOp::INT_ADD;
        Address.Addr = Op.Addr;
        Address.Output = Fresh();
        Address.addInput(SP);
        Address.addInput(
            MedVar::makeConst(V2 ? ELFv2TOCSaveOffset : TOCSaveOffset, WordSize,
                              ConstantAddressProvenance::Scalar));
        MedOp Store;
        Store.Opcode = NdOp::STORE;
        Store.Addr = Op.Addr;
        Store.addInput(Address.Output);
        Store.addInput(Saved);
        Ops.push_back(std::move(Address));
        Ops.push_back(std::move(Store));
        if (RestoresTOC)
          Restores.emplace(Resume, std::pair(Block.Id, Saved));
      } else if (RestoresTOC)
        Calls.CallerRestores.try_emplace(Resume, Op.CallSiteId);
    }
    if (!Ops.empty()) {
      Ops.insert(Ops.end(), std::make_move_iterator(Block.Ops.begin() + First),
                 std::make_move_iterator(Block.Ops.end()));
      Block.Ops = std::move(Ops);
    }
  }
  // A canonical immediate reload on the call's sole returning edge reads
  // the value the stub just saved. Preserve the physical store for code that
  // also observes the linkage area, and leave alternate-entry loads intact.
  if (!Restores.empty())
    for (auto &Block : Function.Blocks)
      for (auto &Op : Block.Ops) {
        if (Op.Opcode != NdOp::LOAD || Op.Output.Size != WordSize)
          continue;
        auto It = Restores.find(Op.Addr);
        if (It == Restores.end())
          continue;
        auto &[Caller, Saved] = It->second;
        if (Block.Id != Caller &&
            (Block.StartAddr != Op.Addr || Block.Preds.size() != 1 ||
             Block.Preds.front() != Caller || !Block.ExceptionalPreds.empty()))
          continue;
        Op.Opcode = NdOp::COPY;
        Op.Inputs[0] = Saved;
      }
  return Calls;
}

void recoverPPC64ELFCalls(MedFunc &Function, const BinaryImage *Image,
                          const PPC64LinkageCalls &Calls, bool Descriptors) {
  using namespace ppc_elf;
  if (Image)
    recoverCallerTOCRestores(Function, *Image, Calls);
  MedDefinitionIndex Defs(Function);
  const auto TOCReg = integerreg::gpr(TOCRegister);
  const auto EnvReg = integerreg::gpr(EnvironmentRegister);
  llvm::DenseSet<std::pair<int, int>> ClobberedTOCs;
  if (!Descriptors)
    for (const auto &C : Function.CallClobbers)
      if (C.Value.Kind == MedVar::Reg && C.Value.RegOff == TOCReg)
        ClobberedTOCs.insert({C.Value.Id, C.Value.SSAVer});
  auto CheckTOC = [&](const MedVar &V) {
    if (V.Kind == MedVar::Reg && V.RegOff == TOCReg &&
        ClobberedTOCs.contains({V.Id, V.SSAVer}))
      throw LowToMedConversionError(
          "PPC64 ELFv2 call leaves the TOC undefined at its use");
  };
  std::optional<va_t> EntryTOC;
  if (Image) {
    const auto It =
        Image->PPC64EntryContexts.find(Image->ppc64GlobalEntry(Function.Entry));
    if (It != Image->PPC64EntryContexts.end())
      EntryTOC = It->second.TOC;
  }
  for (auto &Block : Function.Blocks) {
    if (!ClobberedTOCs.empty()) {
      for (const auto &Phi : Block.Phis)
        for (const auto &Arg : Phi.Args)
          CheckTOC(Arg.second);
      for (const auto &Op : Block.Ops)
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          CheckTOC(Op.Inputs[I]);
    }
    if (llvm::none_of(Block.Ops, [](const MedOp &Op) {
          return Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
        }))
      continue;
    llvm::SmallPtrSet<const MedOp *, 8> Loads;
    std::optional<MedVar> TOC, Env;
    std::optional<MedVar> IndirectEntry;
    auto TrackDescriptorState = [&](const MedOp &Op) {
      if (Op.Opcode == NdOp::LOAD && Op.Output.Size == WordSize &&
          Op.NumInputs == 1 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Op.MemoryOrdering == NdMemoryOrdering::None)
        Loads.insert(&Op);
      if ((opcodeSupportsMemoryAddressSpace(Op.Opcode) &&
           Op.Opcode != NdOp::LOAD) ||
          (Op.Output.Size && Op.Output.Kind == MedVar::Stack) ||
          Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        Loads.clear();
        if (Op.Opcode != NdOp::STORE) {
          TOC.reset();
          Env.reset();
        }
      }
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Size == WordSize) {
        if (Op.Output.RegOff == TOCReg)
          TOC = Op.Output;
        if (Op.Output.RegOff == EnvReg)
          Env = Op.Output;
      }
    };
    // The predicate split moves only the call. Its one guard predecessor
    // still owns the reaching descriptor loads and any intervening clobbers.
    if (Descriptors && Block.Role == MedBlockRole::PredicatedEffect &&
        Block.Preds.size() == 1)
      for (const MedOp &Op : Function.Blocks[Block.Preds.front()].Ops)
        TrackDescriptorState(Op);
    for (auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::COPY && Op.Output.Kind == MedVar::Temp) {
        if (Calls.IndirectEntrySnapshots.contains(Op.Output.Id))
          IndirectEntry = stripCopies(Op.Inputs[0], Defs);
        auto Need = Calls.RequiredTOCs.find(Op.Output.Id);
        if (Need != Calls.RequiredTOCs.end() &&
            tocAddress(Op.Inputs[0], Defs, EntryTOC,
                       Descriptors
                           ? std::nullopt
                           : std::optional(Function.Entry)) != Need->second)
          throw LowToMedConversionError(
              "PPC64 direct call crosses unproven TOC contexts");
        auto Global = Calls.RequiredGlobalEntries.find(Op.Output.Id);
        if (Global != Calls.RequiredGlobalEntries.end() &&
            tocAddress(Op.Inputs[0], Defs, EntryTOC, Global->second) !=
                Global->second)
          throw LowToMedConversionError(
              "PPC64 ELFv2 global call requires r12 to name its entry");
      }
      if (Op.Opcode == NdOp::CALL)
        if (auto It = Calls.CallTargets.find(Op.CallSiteId);
            It != Calls.CallTargets.end())
          Op.Inputs[0] = MedVar::makeConst(
              It->second, WordSize, ConstantAddressProvenance::CodeAddress);
      if (!Descriptors && Op.Opcode == NdOp::INDIR_CALL) {
        const auto *Mask = Defs.find(stripCopies(Op.Inputs[0], Defs));
        if (!IndirectEntry || !Mask || Mask->Opcode != NdOp::INT_AND ||
            Mask->NumInputs != 2 || !Mask->Inputs[1].isConst() ||
            Mask->Inputs[1].ConstVal != ~(uint64_t(InstructionAlignment) - 1) ||
            stripCopies(Mask->Inputs[0], Defs) != *IndirectEntry)
          throw LowToMedConversionError("PPC64 ELFv2 indirect call requires "
                                        "the same global entry in CTR and r12");
        Op.Inputs[0] = *IndirectEntry;
        IndirectEntry.reset();
      }
      if (!Descriptors)
        continue;
      if (Op.Opcode == NdOp::INDIR_CALL) {
        const auto *Mask = Defs.find(stripCopies(Op.Inputs[0], Defs));
        std::optional<MedVar> Base;
        if (Mask && Mask->Opcode == NdOp::INT_AND && Mask->NumInputs == 2 &&
            Mask->Inputs[1].isConst() &&
            Mask->Inputs[1].Provenance == ConstantAddressProvenance::Scalar &&
            Mask->Inputs[1].ConstVal == ~(uint64_t(InstructionAlignment) - 1))
          Base = fieldBase(Mask->Inputs[0], EntryOffset, Defs, Loads);
        if (!Base || !TOC || !Env ||
            fieldBase(*TOC, TOCOffset, Defs, Loads) != Base ||
            fieldBase(*Env, EnvironmentOffset, Defs, Loads) != Base)
          throw LowToMedConversionError(
              "PPC64 ELFv1 indirect call has no proven function descriptor");
        if (Image)
          checkDescriptorInitializer(*Base, Defs, *Image, EntryTOC);
        Op.Inputs[0] = *Base;
      }

      TrackDescriptorState(Op);
    }
  }
  if (EntryTOC)
    bindTOCAddresses(Function, Defs, *Image, *EntryTOC);
}
} // namespace neverd
