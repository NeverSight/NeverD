//===- ImportCallee.h - The import a call address names --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The import that a call to an address reaches, for the register summaries
/// and the prototypes that fix an import's entry reads.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_IMPORTCALLEE_H
#define NEVERD_IR_LOW_IMPORTCALLEE_H

#include "neverd/Limits.h"
#include "neverd/ir/FloatingCallArguments.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/libc/LibCResult.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace neverd {

/// The name of the import that a call to \p Addr reaches: through an
/// executable stub or an import address slot the import directory lists, or
/// through a pointer slot the loader binds to the import (an ELF GLOB_DAT or
/// JUMP_SLOT relocation, a Mach-O bind) with no addend.  Empty when \p Addr
/// names no import, as for a slot holding `import + addend` or one that two
/// imports claim.
inline std::string importCalleeName(const BinaryImage &Img, va_t Addr) {
  if (Img.functionHasBytePatch(Addr))
    return {};
  const ImportStorageSlotCollection Storage =
      Img.collectImportStorageSlot(Addr);
  if (Storage.Conflicts.count(Addr))
    return {};
  const auto It = Storage.Slots.find(Addr);
  if (It != Storage.Slots.end() && It->second.Addend != 0)
    return {};
  if (const Import *Imp = Img.findImportAt(Addr); Imp && !Imp->Name.empty()) {
    if (Imp->IATAddr == Addr && It != Storage.Slots.end() &&
        It->second.Name != Imp->Name)
      return {};
    return Imp->Name;
  }
  if (It != Storage.Slots.end())
    return It->second.Name;
  return {};
}

/// The C name of the import a call to \p Addr reaches, through its stub or
/// its slot, or empty.
inline std::string importCalleeCName(const BinaryImage &Img, va_t Addr) {
  if (Img.functionHasBytePatch(Addr))
    return {};
  std::string Name = importCalleeName(Img, Addr);
  if (Name.empty())
    if (const Import *Imp = Img.findImportStubAt(Addr))
      Name = Imp->Name;
  return Name.empty() ? Name : cNameOfSymbol(Name, Img.Format, Img.Arch).str();
}

/// Whether the import a call to \p Addr reaches, through its stub or its
/// slot, is a compiler runtime routine that returns a double word on a target
/// of \p WordBytes registers (libc::returnsDoubleWord): `__udivti3` returns
/// RDX:RAX on x86-64.
inline bool importReturnsDoubleWord(const BinaryImage &Img, va_t Addr,
                                    unsigned WordBytes) {
  const std::string Name = importCalleeCName(Img, Addr);
  return !Name.empty() && libc::returnsDoubleWord(Name, WordBytes);
}

/// Whether the import a call to \p Addr reaches returns no record of
/// registers, as its C declaration says: nothing, or a pointer.  Neither the
/// call nor a stub that jumps to the import returns more, whatever its
/// callers read after it.
inline bool importDeclaresNoRecord(const BinaryImage &Img, va_t Addr) {
  const std::string Name = importCalleeCName(Img, Addr);
  if (Name.empty())
    return false;
  const std::optional<libc::LibCResult> Result =
      libc::libcResult(Name, libc::libcVariantOf(Img));
  return Result && *Result != libc::LibCResult::Value;
}

/// Every address a call reaches an import through: its executable stubs
/// and its import address slots.
inline std::set<va_t> importCallAddresses(const BinaryImage &Img) {
  std::set<va_t> Addresses;
  for (const auto &[Addr, Name] : Img.getImportAddressNames())
    Addresses.insert(Addr);
  return Addresses;
}

/// The constant address the value \p Var holds at op \p Index of \p Block
/// was loaded from: its last write before there, in the block or the chain
/// of single predecessors above it among \p Blocks, is a load from an address
/// that folds to a constant (`mov rax, [rip+slot]`).
struct LoadedCallSlotProof {
  std::function<std::optional<SourceFrameDefinition>(int, size_t)> Reload;
  std::function<bool(const LowOp &, const NdVar &)> Preserves;
  size_t *WorkRemaining = nullptr;
  /// An observational query may retain and cross exact alignment checks.
  /// Ordinary callable/relocation proofs keep their existing refusal.
  bool AllowAlignmentChecks = false;
};

/// The register an instruction keeps the bits of under a mask, and the
/// constant bits it sets over the rest, when Ops[\p Index] of \p Block ends
/// it as `r = (r & mask) | bits`: ARM's movt, whose `lo = r & 0xffff` and
/// `hi = imm16 << 16` it ORs, writes the high half of an address movw set.
struct MergedRegisterBits {
  NdVar Register;
  uint64_t KeptMask = 0;
  uint64_t Bits = 0;
};

inline std::optional<MergedRegisterBits>
mergedRegisterBits(const LowBlock &Block, size_t Index) {
  if (Index >= Block.Ops.size())
    return std::nullopt;
  const LowOp &Or = Block.Ops[Index];
  if (Or.Opcode != NdOp::INT_OR || Or.NumInputs != 2 || !Or.Output.isReg() ||
      !Or.Output.Size || Or.Output.Size > 8 || !Or.Inputs[0].isTemp() ||
      !Or.Inputs[1].isTemp() || Or.Inputs[0].Offset == Or.Inputs[1].Offset)
    return std::nullopt;
  // The last write of each temporary before the OR, in its instruction.
  const LowOp *Defs[2] = {nullptr, nullptr};
  for (size_t J = Index; J-- > 0 && Block.Ops[J].Addr == Or.Addr;)
    for (unsigned I = 0; I < 2; ++I)
      if (!Defs[I] && Block.Ops[J].Output.isTemp() &&
          Block.Ops[J].Output.Offset == Or.Inputs[I].Offset &&
          Block.Ops[J].Output.Size == Or.Inputs[I].Size)
        Defs[I] = &Block.Ops[J];
  if (!Defs[0] || !Defs[1])
    return std::nullopt;
  const uint64_t Width = Or.Output.Size == 8
                             ? ~uint64_t{0}
                             : (uint64_t{1} << (8 * Or.Output.Size)) - 1;
  for (unsigned I = 0; I < 2; ++I) {
    const LowOp &And = *Defs[I];
    const LowOp &Set = *Defs[1 - I];
    if (And.Opcode != NdOp::INT_AND || And.NumInputs != 2 ||
        !And.Inputs[0].isReg() || And.Inputs[0].Offset != Or.Output.Offset ||
        And.Inputs[0].Size != Or.Output.Size || !And.Inputs[1].isConst())
      continue;
    std::optional<uint64_t> Bits;
    if (Set.Opcode == NdOp::COPY && Set.NumInputs == 1 &&
        Set.Inputs[0].isConst())
      Bits = Set.Inputs[0].Offset;
    else if (Set.Opcode == NdOp::INT_LEFT && Set.NumInputs == 2 &&
             Set.Inputs[0].isConst() && Set.Inputs[1].isConst() &&
             Set.Inputs[1].Offset < 8 * Or.Output.Size)
      Bits = Set.Inputs[0].Offset << Set.Inputs[1].Offset;
    if (!Bits)
      continue;
    const uint64_t KeptMask = And.Inputs[1].Offset & Width;
    // A bit both kept and set ORs into the kept value: two halves do not
    // merge so.
    if (*Bits & Width & KeptMask)
      continue;
    return MergedRegisterBits{Or.Output, KeptMask, *Bits & Width};
  }
  return std::nullopt;
}

inline std::optional<va_t>
loadedCallSlot(llvm::ArrayRef<LowBlock> Blocks, const LowBlock &Block,
               size_t Index, NdVar Var,
               const LoadedCallSlotProof *Proof = nullptr) {
  auto Same = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
  };
  const LowBlock *Cur = &Block;
  if (Index >= Block.Ops.size())
    return std::nullopt;
  va_t Instruction = Block.Ops[Index].Addr;
  size_t End = Index;
  bool Loaded = false;
  uint64_t Addend = 0;
  unsigned LiteralBits = 0;
  // The bits an instruction set over the address the rest of the walk
  // reaches, which it kept under the mask (mergedRegisterBits).
  std::optional<MergedRegisterBits> Merged;
  for (int Step = 0; Step < limits::kCallTargetSlotDepth; ++Step) {
    if (Loaded && Var.isConst()) {
      uint64_t Value = LiteralBits
                           ? Var.Offset & ((UINT64_C(1) << LiteralBits) - 1)
                           : Var.Offset;
      if (Merged)
        Value = (Value & Merged->KeptMask) | Merged->Bits;
      return static_cast<va_t>(Value + Addend);
    }
    if (!Var.isReg() && !Var.isTemp())
      return std::nullopt;
    const LowOp *Def = nullptr;
    for (size_t J = End; J-- > 0;) {
      if (Proof && Proof->WorkRemaining) {
        if (!*Proof->WorkRemaining)
          return std::nullopt;
        --*Proof->WorkRemaining;
      }
      const LowOp &Candidate = Cur->Ops[J];
      // LowIR temporaries belong to one instruction. Intrinsics require a
      // separate observational opt-in; calls require preservation evidence.
      if ((Var.isTemp() && Candidate.Addr != Instruction) ||
          (Candidate.Opcode == NdOp::INTRINSIC &&
           (!Proof || !Proof->AllowAlignmentChecks ||
            !isSourceFrameObservationGuard(Candidate))))
        return std::nullopt;
      if (Candidate.Opcode == NdOp::CALL ||
          Candidate.Opcode == NdOp::INDIR_CALL) {
        if (!Proof || !Proof->Preserves || !Proof->Preserves(Candidate, Var))
          return std::nullopt;
        continue;
      }
      const NdVar &Written = Candidate.Output;
      if (!Written.Size || Written.Space != Var.Space)
        continue;
      if (Written.Offset > UINT64_MAX - Written.Size ||
          Var.Offset > UINT64_MAX - Var.Size)
        return std::nullopt;
      if (Var.isTemp() ? Written.Offset != Var.Offset
                       : (Written.Offset >= Var.Offset + Var.Size ||
                          Var.Offset >= Written.Offset + Written.Size))
        continue;
      if (!Same(Written, Var))
        return std::nullopt;
      Def = &Candidate;
      End = J;
      Instruction = Candidate.Addr;
      break;
    }
    if (!Def) {
      if (Var.isTemp() || Cur->Preds.size() != 1 ||
          !Cur->ExceptionalPreds.empty())
        return std::nullopt;
      const int Pred = Cur->Preds.front();
      auto It = std::find_if(Blocks.begin(), Blocks.end(),
                             [&](const LowBlock &B) { return B.Id == Pred; });
      if (It == Blocks.end())
        return std::nullopt;
      Cur = &*It;
      End = Cur->Ops.size();
      continue;
    }
    if (Def->NumInputs < 1)
      return std::nullopt;
    if (!Loaded && Def->Opcode == NdOp::LOAD) {
      if (Proof && Proof->Reload && !Def->Inputs[0].isConst())
        if (const auto Source = Proof->Reload(Cur->Id, End)) {
          const auto Producer = std::find_if(
              Blocks.begin(), Blocks.end(),
              [&](const LowBlock &B) { return B.Id == Source->BlockId; });
          if (Producer == Blocks.end() ||
              Source->OperationIndex >= Producer->Ops.size())
            return std::nullopt;
          const auto &Original = Producer->Ops[Source->OperationIndex];
          if (Original.Addr != Source->Instruction ||
              Original.Seq != Source->Sequence ||
              Original.Output != Source->Output ||
              Original.Output.Size != Var.Size)
            return std::nullopt;
          Cur = &*Producer;
          End = Source->OperationIndex + 1;
          Instruction = Original.Addr;
          Var = Original.Output;
          continue;
        }
      Loaded = true;
      Var = Def->Inputs[0];
      continue;
    }
    if (Def->Opcode == NdOp::COPY && (Loaded || !Def->Inputs[0].isConst())) {
      if (LiteralBits && Def->Inputs[0].Size != Def->Output.Size)
        return std::nullopt;
      Var = Def->Inputs[0];
      continue;
    }
    // The i386 lifter widens a literal 32-bit memory address to its 64-bit
    // address temporary. Trace only same-width copies back to that literal;
    // arithmetic inside the extension needs its own width-aware proof.
    // An extension of the loaded callable itself is never accepted here.
    if (Loaded && !LiteralBits && !Merged && Def->Opcode == NdOp::INT_ZEXT &&
        Def->NumInputs == 1 && Def->Inputs[0].Size > 0 &&
        Def->Inputs[0].Size < Def->Output.Size && Def->Output.Size <= 8) {
      LiteralBits = Def->Inputs[0].Size * 8;
      Var = Def->Inputs[0];
      continue;
    }
    if (Loaded && Def->Opcode == NdOp::INT_ADD && Def->NumInputs == 2 &&
        Def->Inputs[1].isConst() && !LiteralBits && !Merged) {
      Addend += Def->Inputs[1].Offset;
      Var = Def->Inputs[0];
      continue;
    }
    // An address whose high half ARM's movt wrote over the half movw set
    // (`movw r12, #lo; movt r12, #hi; ldr pc, [r12]`, an MSVC import
    // thunk): the walk follows the register the movt kept the low half of.
    // An addition the walk meets after would act on the kept half alone.
    if (Loaded && !LiteralBits && !Merged && Def->Output.Size == Var.Size)
      if (const std::optional<MergedRegisterBits> Bits =
              mergedRegisterBits(*Cur, End)) {
        Merged = *Bits;
        Var = Bits->Register;
        continue;
      }
    return std::nullopt;
  }
  return std::nullopt;
}

inline std::optional<va_t>
loadedCallSlot(const LowFunc &F, const LowBlock &Block, size_t Index, NdVar Var,
               const LoadedCallSlotProof *Proof = nullptr) {
  return loadedCallSlot(llvm::ArrayRef<LowBlock>(F.Blocks), Block, Index, Var,
                        Proof);
}

struct ImportedCallSlotFacts {
  using Operation = std::pair<int, size_t>;
  std::map<Operation, va_t> Calls;
  std::map<Operation, SourceFrameDefinition> Reloads;
};

/// Extend ordinary import-slot lookup through preserved registers and exact
/// private spills. Complete independently authenticated declarations own call
/// effects; shared SourceFrameAnalysis owns reaching bytes and escape checks.
/// These facts do not replace a cached runtime pointer with a fresh IAT load.
ImportedCallSlotFacts
importedCallSlotFacts(const LowFunc &Function, const BinaryImage &Image,
                      const std::map<va_t, FloatingCallArguments> &Signatures,
                      const std::map<va_t, va_t> *ArgumentSources = nullptr);

/// The routine whose arguments the indirect call \p Op passes, as
/// \p ArgumentSources keys it by the address of the function-pointer variable
/// the call goes through (collectPointerVariableCallees): the call's constant
/// slot, or the slot its target register was loaded from.
inline std::optional<va_t>
pointerVariableArgumentsFrom(const std::map<va_t, va_t> *ArgumentSources,
                             llvm::ArrayRef<LowBlock> Blocks,
                             const LowBlock &Block, size_t Index,
                             const LowOp &Op) {
  if (!ArgumentSources || ArgumentSources->empty() ||
      Op.Opcode != NdOp::INDIR_CALL || Op.NumInputs == 0)
    return std::nullopt;
  std::optional<va_t> Slot;
  if (Op.Inputs[0].isConst())
    Slot = Op.Inputs[0].Offset;
  else if (Op.Inputs[0].isReg())
    Slot = loadedCallSlot(Blocks, Block, Index, Op.Inputs[0]);
  if (!Slot)
    return std::nullopt;
  const auto It = ArgumentSources->find(*Slot);
  if (It == ArgumentSources->end())
    return std::nullopt;
  return It->second;
}

/// The routine of this image a call to an import's stub or through its slot,
/// \p Target, reaches: the loader starts the slot of a symbol the image
/// defines itself at that routine (ownRoutineOfImport), which
/// \p ArgumentSources keys the slot to.  The call passes that routine's
/// arguments, not those of another module's routine its import would name,
/// unless the C library tables declare the import's own: an image's copy of
/// the unwinder's _Unwind_RaiseException takes what its declaration says,
/// which a summary of the copy's body only approximates.
inline std::optional<va_t>
importOwnRoutine(const BinaryImage &Img,
                 const std::map<va_t, va_t> *ArgumentSources, va_t Target) {
  if (!ArgumentSources || ArgumentSources->empty())
    return std::nullopt;
  // A slot is one a function-pointer variable's source names; any other
  // target, the stub its registration names.  Few calls reach either, so
  // neither lookup scans the import table for most.
  const Import *Imp = nullptr;
  if (ArgumentSources->count(Target)) {
    Imp = Img.findImportAt(Target);
    if (Imp && Imp->IATAddr != Target)
      Imp = nullptr;
  } else {
    Imp = Img.findImportStubAt(Target);
  }
  if (!Imp || !Imp->IATAddr)
    return std::nullopt;
  if (libc::libcArityForSymbol(Imp->Name, libc::libcVariantOf(Img)) ||
      libc::libcPrototype(cNameOfStatedName(Imp->Name, Img.Format, Img.Arch),
                          Img.abiFormat()))
    return std::nullopt;
  const auto It = ArgumentSources->find(Imp->IATAddr);
  if (It == ArgumentSources->end() || It->second == Imp->IATAddr)
    return std::nullopt;
  return It->second;
}

} // namespace neverd

#endif // NEVERD_IR_LOW_IMPORTCALLEE_H
