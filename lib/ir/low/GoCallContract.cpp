//===- GoCallContract.cpp - Registers a call from Go code keeps -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Go code has no callee-saved registers, and its runtime write barriers keep
/// every general-purpose register they do not return in.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/GoCallContract.h"

#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace neverd {
namespace {

/// Whether \p Register is a general-purpose register of Go's on \p A, which
/// a write barrier keeps: RAX through R15, or X0 through X30.
bool isGoGeneralRegister(Arch A, uint64_t Register) {
  if (A == Arch::X64)
    return Register <= x86reg::R15;
  if (A == Arch::AArch64)
    return Register <= a64reg::X30;
  return false;
}

/// Whether \p Address lies in one of the sorted, disjoint \p Ranges.
bool inRanges(const std::vector<ExceptionAddressRange> &Ranges, va_t Address) {
  const auto After = std::upper_bound(
      Ranges.begin(), Ranges.end(), Address,
      [](va_t A, const ExceptionAddressRange &R) { return A < R.Begin; });
  return After != Ranges.begin() && std::prev(After)->contains(Address);
}

/// A register plus a constant: what an address the block forms names.
struct BasePlus {
  uint64_t Base = 0;
  int64_t Offset = 0;
};

/// Follows COPY, INT_ADD and INT_SUB of a full-width register through one
/// block's temporaries, so that the address a store or load uses reads as
/// its base register plus a constant.  A register the block overwrites with
/// anything else no longer names itself.
class BaseTracker {
  std::map<std::pair<VnodeSpace, uint64_t>, BasePlus> Known;
  std::set<uint64_t> Overwritten;

  static std::pair<VnodeSpace, uint64_t> key(const NdVar &V) {
    return {V.Space, V.Offset};
  }

public:
  std::optional<BasePlus> of(const NdVar &V) const {
    if (const auto It = Known.find(key(V)); It != Known.end())
      return It->second;
    if (V.isReg() && V.Size == 8 && !Overwritten.count(V.Offset))
      return BasePlus{V.Offset, 0};
    return std::nullopt;
  }

  void step(const LowOp &Op) {
    if (!Op.Output.isReg() && !Op.Output.isTemp())
      return;
    std::optional<BasePlus> Value;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 && Op.Output.Size == 8)
      Value = of(Op.Inputs[0]);
    else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
             Op.NumInputs == 2 && Op.Inputs[1].isConst() && Op.Output.Size == 8)
      if (const std::optional<BasePlus> Base = of(Op.Inputs[0])) {
        const int64_t Delta = static_cast<int64_t>(Op.Inputs[1].Offset);
        Value = BasePlus{Base->Base, Op.Opcode == NdOp::INT_ADD
                                         ? Base->Offset + Delta
                                         : Base->Offset - Delta};
      }
    if (Value) {
      Known[key(Op.Output)] = *Value;
      return;
    }
    Known.erase(key(Op.Output));
    if (Op.Output.isReg())
      Overwritten.insert(Op.Output.Offset);
  }
};

} // namespace

bool isGoCode(const BinaryImage &Img, va_t Address) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  if (!Module || (Img.Arch != Arch::X64 && Img.Arch != Arch::AArch64))
    return false;
  return inRanges(Module->GoCode, Address);
}

const GoWriteBarrier *goWriteBarrierAt(const BinaryImage &Img, va_t Entry) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  if (!Module)
    return nullptr;
  const auto &Barriers = Module->WriteBarriers;
  const auto It = std::lower_bound(
      Barriers.begin(), Barriers.end(), Entry,
      [](const GoWriteBarrier &B, va_t A) { return B.Entry < A; });
  return It != Barriers.end() && It->Entry == Entry ? &*It : nullptr;
}

std::optional<uint64_t> goWriteBarrierResult(Arch A, GoWriteBarrierKind Kind) {
  if (Kind != GoWriteBarrierKind::Buffer)
    return std::nullopt;
  if (A == Arch::X64)
    return x86reg::R11;
  if (A == Arch::AArch64)
    return a64reg::X25;
  return std::nullopt;
}

CallRegisterContract callRegisterContractOf(const BinaryImage &Img, va_t Caller,
                                            va_t Callee) {
  if (!isGoCode(Img, Caller))
    return CallRegisterContract::Platform;
  if (Callee != InvalidVA && goWriteBarrierAt(Img, Callee))
    return CallRegisterContract::GoWriteBarrier;
  return CallRegisterContract::GoCode;
}

std::optional<uint64_t> goCallResultRegister(const BinaryImage &Img,
                                             CallRegisterContract Contract,
                                             va_t Callee) {
  if (Contract != CallRegisterContract::GoWriteBarrier)
    return std::nullopt;
  const GoWriteBarrier *Barrier = goWriteBarrierAt(Img, Callee);
  return Barrier ? goWriteBarrierResult(Img.Arch, Barrier->Kind) : std::nullopt;
}

bool usesGoRegisterArguments(const BinaryImage &Img, va_t Entry) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  return Module && Module->RegisterArguments && Img.Arch == Arch::X64 &&
         isGoCode(Img, Entry) && !inRanges(Module->GoAssembly, Entry);
}

llvm::ArrayRef<uint64_t> goIntegerArgumentRegisters(Arch A) {
  static constexpr uint64_t X64[] = {x86reg::RAX, x86reg::RBX, x86reg::RCX,
                                     x86reg::RDI, x86reg::RSI, x86reg::R8,
                                     x86reg::R9,  x86reg::R10, x86reg::R11};
  if (A == Arch::X64)
    return X64;
  return {};
}

std::optional<std::vector<GoArgumentSpill>>
goMorestackSpills(const BinaryImage &Img, llvm::ArrayRef<LowBlock> Blocks) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  if (!Module || Img.Arch != Arch::X64 || Blocks.empty() ||
      Blocks.front().Ops.empty())
    return std::nullopt;
  // The entry compares the stack pointer with the goroutine's stack guard,
  // `g.stackguard0` at 16(R14), and branches to the growth path below it.
  const LowBlock &Entry = Blocks.front();
  BaseTracker EntryValues;
  bool ReadsGuard = false;
  for (const LowOp &Op : Entry.Ops) {
    if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
      if (const std::optional<BasePlus> Address = EntryValues.of(Op.Inputs[0]);
          Address && Address->Base == x86reg::R14 && Address->Offset == 0x10)
        ReadsGuard = true;
    EntryValues.step(Op);
  }
  const LowOp &Check = Entry.Ops.back();
  if (!ReadsGuard || Check.Opcode != NdOp::COND_BR || Check.NumInputs < 1 ||
      !Check.Inputs[0].isConst())
    return std::nullopt;
  const auto Growth =
      llvm::find_if(Blocks.drop_front(), [&](const LowBlock &Block) {
        return Block.StartAddr == Check.Inputs[0].Offset;
      });
  if (Growth == Blocks.end())
    return std::nullopt;

  // Spill, call morestack, reload what was spilled, start again.
  BaseTracker Values;
  std::vector<GoArgumentSpill> Spills, Reloads;
  std::map<uint64_t, std::pair<int64_t, uint16_t>> Loaded;
  enum class Phase { Spilling, Reloading, Restarted } At = Phase::Spilling;
  for (const LowOp &Op : Growth->Ops) {
    switch (Op.Opcode) {
    case NdOp::STORE: {
      const std::optional<BasePlus> Slot =
          Op.NumInputs >= 2 ? Values.of(Op.Inputs[0]) : std::nullopt;
      if (At != Phase::Spilling || !Slot || Slot->Base != x86reg::RSP ||
          Slot->Offset < 8 || !Op.Inputs[1].isReg())
        return std::nullopt;
      Spills.push_back({Op.Inputs[1].Offset, Op.Inputs[1].Size, Slot->Offset});
      break;
    }
    case NdOp::CALL:
      if (At != Phase::Spilling || Op.NumInputs < 1 ||
          !Op.Inputs[0].isConst() ||
          !std::binary_search(Module->MorestackEntries.begin(),
                              Module->MorestackEntries.end(),
                              Op.Inputs[0].Offset))
        return std::nullopt;
      At = Phase::Reloading;
      break;
    case NdOp::LOAD: {
      const std::optional<BasePlus> Slot =
          Op.NumInputs >= 1 ? Values.of(Op.Inputs[0]) : std::nullopt;
      if (At != Phase::Reloading || !Slot || Slot->Base != x86reg::RSP)
        return std::nullopt;
      if (Op.Output.isReg())
        Reloads.push_back({Op.Output.Offset, Op.Output.Size, Slot->Offset});
      else if (Op.Output.isTemp())
        Loaded[Op.Output.Offset] = {Slot->Offset, Op.Output.Size};
      else
        return std::nullopt;
      break;
    }
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      // A reload may widen what it loads (`movzx ecx, byte [rsp+0x18]`): it
      // restores the bytes the spill wrote, through temporaries, and a 32-bit
      // write then clears the register's upper half.
      if (At == Phase::Reloading && Op.NumInputs == 1) {
        if (Op.Inputs[0].isTemp())
          if (const auto It = Loaded.find(Op.Inputs[0].Offset);
              It != Loaded.end()) {
            if (Op.Output.isTemp())
              Loaded[Op.Output.Offset] = It->second;
            else if (Op.Output.isReg())
              Reloads.push_back(
                  {Op.Output.Offset, It->second.second, It->second.first});
          }
        if (Op.Opcode != NdOp::COPY && Op.Inputs[0].isReg() &&
            !(Op.Output.isReg() && Op.Output.Offset == Op.Inputs[0].Offset))
          return std::nullopt;
      } else if (Op.Opcode != NdOp::COPY) {
        return std::nullopt;
      }
      break;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      break;
    case NdOp::BRANCH:
      if (At != Phase::Reloading || Op.NumInputs < 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Offset != Entry.StartAddr)
        return std::nullopt;
      At = Phase::Restarted;
      break;
    default:
      return std::nullopt;
    }
    Values.step(Op);
  }
  if (At != Phase::Restarted)
    return std::nullopt;
  const auto BySlot = [](const GoArgumentSpill &A, const GoArgumentSpill &B) {
    return std::tie(A.EntryOffset, A.Register, A.Size) <
           std::tie(B.EntryOffset, B.Register, B.Size);
  };
  llvm::sort(Spills, BySlot);
  llvm::sort(Reloads, BySlot);
  const auto Same = [](const GoArgumentSpill &A, const GoArgumentSpill &B) {
    return A.Register == B.Register && A.Size == B.Size &&
           A.EntryOffset == B.EntryOffset;
  };
  if (Spills.size() != Reloads.size() ||
      !std::equal(Spills.begin(), Spills.end(), Reloads.begin(), Same))
    return std::nullopt;
  for (size_t I = 1; I < Spills.size(); ++I)
    if (Spills[I].EntryOffset < Spills[I - 1].EntryOffset + Spills[I - 1].Size)
      return std::nullopt;
  return Spills;
}

uint16_t goContractKeptBytes(const TargetRegInfo &TRI,
                             CallRegisterContract Contract, uint64_t RegOff,
                             uint16_t Size, std::optional<uint64_t> Result) {
  const uint16_t Width = TRI.FullRegWidth;
  if (Size == 0 || Width == 0 || RegOff % Width + Size > Width)
    return 0;
  const uint64_t Register = RegOff - RegOff % Width;
  // The stack pointer and Go's frame pointer keep their values across any
  // call ("RSP | Stack pointer | Same", "RBP | Frame pointer | Same" in
  // abi-internal.md, likewise RSP and R29 on arm64).
  if (TRI.isFrameReg(Register))
    return Size;
  if (Contract != CallRegisterContract::GoWriteBarrier ||
      !isGoGeneralRegister(TRI.TheArch, Register) ||
      (Result && Register == *Result) ||
      (TRI.TheArch == Arch::AArch64 && Register == a64reg::X27))
    return 0;
  return Size;
}

} // namespace neverd
