//===- CallRegisterEffects.cpp - Callee GPR write summaries -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CallRegisterEffects.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/ir/intrinsics/X86/X64Syscall.h"
#include "neverd/ir/low/BlockRegisterConstant.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/libc/StackProbeIndex.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ELF/ELFOperatingSystem.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <array>
#include <deque>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

namespace {
constexpr uint64_t kX64GPRBytes = 16 * 8;
constexpr unsigned kX64StackPointerFamily = 4; // RSP

GPRFamilyMask familyBit(Arch A, uint64_t RegOff) {
  auto Family = registerFamilyOf(A, RegOff);
  if (!Family || Family->first == kX64StackPointerFamily)
    return 0;
  return GPRFamilyMask(1) << Family->first;
}

/// The bytes a family's register holds.
unsigned familyBytes(unsigned Family) {
  return Family >= kX64VectorFamilyBase ? 16 : 8;
}

/// Record a read of \p Size bytes at \p RegOff.
void addRead(Arch A, GPRReadWidths &Reads, uint64_t RegOff, uint64_t Size) {
  auto Family = registerFamilyOf(A, RegOff);
  if (!Family || Family->first == kX64StackPointerFamily)
    return;
  const uint64_t End =
      std::min<uint64_t>(Family->second + Size, familyBytes(Family->first));
  Reads[Family->first] =
      std::max(Reads[Family->first], static_cast<uint8_t>(End));
}

/// The vector argument family \p V names as a whole register, if any.
std::optional<unsigned> wholeVectorFamily(Arch A, const NdVar &V) {
  const auto Family = V.isReg() ? registerFamilyOf(A, V.Offset) : std::nullopt;
  if (!Family || Family->first < kX64VectorFamilyBase || Family->second ||
      V.Size != 16)
    return std::nullopt;
  return Family->first;
}

void joinReads(GPRReadWidths &Into, const GPRReadWidths &From) {
  for (size_t I = 0; I < Into.size(); ++I)
    Into[I] = std::max(Into[I], From[I]);
}

/// Copies whose destination is overwritten before any observation in this
/// block. A syntactic read is not an argument use when, for example, XCHG
/// copies an incoming scratch register into RBX just before CPUID defines
/// EBX. Keep all bytes live at block exits and calls: this local proof never
/// assumes a return convention or an omitted callee's register contract.
std::set<size_t>
overwrittenRegisterCopies(Arch A, const LowBlock &Block,
                          const std::map<size_t, uint8_t> &MergeWrites) {
  std::set<size_t> Copies;
  if (A != Arch::X64 || !Block.ExceptionalSuccs.empty())
    return Copies;
  std::array<uint8_t, kX64GPRBytes / 8> Dead{};
  auto Slice =
      [&](const NdVar &V) -> std::optional<std::pair<unsigned, uint8_t>> {
    const auto Family = V.isReg() ? gprFamilyOf(A, V.Offset) : std::nullopt;
    if (!Family || !V.Size || V.Size > 8 || V.Offset % 8 + V.Size > 8)
      return std::nullopt;
    return std::pair{
        *Family, static_cast<uint8_t>(((1u << V.Size) - 1) << (V.Offset % 8))};
  };
  for (size_t I = Block.Ops.size(); I-- > 0;) {
    const LowOp &Op = Block.Ops[I];
    switch (Op.Opcode) {
    case NdOp::CALL:
    case NdOp::INDIR_CALL:
    case NdOp::BRANCH:
    case NdOp::COND_BR:
    case NdOp::INDIR_BR:
    case NdOp::RETURN:
      Dead.fill(0);
      continue;
    default:
      break;
    }
    const auto Output = Slice(Op.Output);
    const bool Unobserved =
        Output && Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
        Op.Inputs[0].isReg() && Op.Output.Size == Op.Inputs[0].Size &&
        Op.MemoryOrdering == NdMemoryOrdering::None &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        !MergeWrites.count(I) &&
        (Dead[Output->first] & Output->second) == Output->second;
    if (Unobserved)
      Copies.insert(I);
    if (Output) {
      uint8_t Written = Output->second;
      if (const auto Merge = MergeWrites.find(I); Merge != MergeWrites.end())
        Written = static_cast<uint8_t>((1u << Merge->second) - 1);
      else if (Op.Output.Offset % 8 == 0 && Op.Output.Size == 4)
        Written = 0xFF; // x86-64's 32-bit writes also define the high half.
      Dead[Output->first] |= Written;
    }
    if (!Unobserved)
      for (unsigned In = 0; In < Op.NumInputs; ++In)
        if (const auto Input = Slice(Op.Inputs[In]))
          Dead[Input->first] &= ~Input->second;
  }
  return Copies;
}

// MSVC 19.44 crashes when these helper types are function-local. Keep their
// definitions private here without changing the save-area recognition.
// An address as a base register family, how often that family had been
// written when the address was formed, and a displacement.
struct Slot {
  unsigned Family = 0;
  unsigned Version = 0;
  int64_t Displacement = 0;
};
struct Spill {
  size_t Block = 0;
  size_t Op = 0;
  Slot Address;
};

/// The stores of \p F, a System V function, that spill the incoming argument
/// registers to the register save area of a variadic prologue, as (block,
/// op) indices.  The prologue reads AL, the count of vector registers the
/// caller set, before writing RAX, branches on it around the vector register
/// spills, and stores the argument registers from the first variadic
/// position through the last, each still unwritten, to consecutive slots off
/// one unchanged base register: in the entry block (GCC, optimizing Clang) or
/// where the branch rejoins (Clang -O0).  Such a spill reads no parameter: a
/// caller passes exactly the variadic arguments it sets.  The vector argument
/// registers spilled in the block a test of AL alone branches around are the
/// save area's too, which \p SavesVectors reports: a caller passes the vector
/// arguments AL counts.
std::set<std::pair<size_t, size_t>>
findRegisterSaveAreaSpills(const BinaryImage &Img, CallRegisterBodyView F,
                           const std::map<int, size_t> &IndexOfId,
                           bool &SavesVectors) {
  std::set<std::pair<size_t, size_t>> Spills;
  SavesVectors = false;
  if (Img.Arch != Arch::X64 || Img.abiFormat() == BinaryFormat::COFF ||
      F.Blocks.empty())
    return Spills;
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  const llvm::ArrayRef<uint64_t> Registers =
      TRI.integerArgumentLayout(Img.abiFormat()).Registers;
  auto IndexOf = [&](int Id) -> std::optional<size_t> {
    if (auto It = IndexOfId.find(Id); It != IndexOfId.end())
      return It->second;
    return std::nullopt;
  };
  auto WritesGPR = [&](const LowBlock &Block) {
    return llvm::any_of(Block.Ops, [&](const LowOp &Op) {
      return Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
             (Op.Output.isReg() && gprFamilyOf(Img.Arch, Op.Output.Offset));
    });
  };
  // The prologue in execution order: the entry block, then, when it branches
  // around a block that writes no integer register and the two paths rejoin
  // with no other way in, the join.
  std::vector<size_t> Prologue{0};
  std::optional<size_t> VectorSpillBlock;
  const LowBlock &Entry = F.Blocks.front();
  if (Entry.Succs.size() == 2)
    if (auto A = IndexOf(Entry.Succs[0]), B = IndexOf(Entry.Succs[1]); A && B)
      for (const auto &[Around, Join] :
           {std::pair{*A, *B}, std::pair{*B, *A}}) {
        const LowBlock &Skipped = F.Blocks[Around];
        if (Around != 0 && Join != 0 && Skipped.Succs.size() == 1 &&
            IndexOf(Skipped.Succs.front()) == Join &&
            Skipped.Preds.size() == 1 && F.Blocks[Join].Preds.size() == 2 &&
            !WritesGPR(Skipped)) {
          Prologue.push_back(Join);
          VectorSpillBlock = Around;
          break;
        }
      }
  std::array<unsigned, kX64GPRBytes / 8> Versions{};
  std::map<uint64_t, Slot> TempSlots;
  GPRFamilyMask Written = 0;
  bool ReadsVectorCount = false;
  // `test al, al` reads AL alone, where an alignment `push rax` reads RAX.
  bool TestsAL = false;
  std::vector<std::optional<Spill>> Stored(Registers.size());
  // The stack slots holding a register's incoming value, by family: GCC's
  // `push rax; pop rax` puts RAX back unchanged before `test al, al`.
  std::map<std::tuple<unsigned, unsigned, int64_t>, unsigned> EntryValueSlots;
  auto SlotOf = [&](const NdVar &V) -> std::optional<Slot> {
    if (V.isTemp()) {
      if (auto It = TempSlots.find(V.Offset); It != TempSlots.end())
        return It->second;
      return std::nullopt;
    }
    if (const auto Family = V.isReg() && V.Size == TRI.PointerSize
                                ? gprFamilyOf(Img.Arch, V.Offset)
                                : std::nullopt)
      return Slot{*Family, Versions[*Family], 0};
    return std::nullopt;
  };
  // The temporaries a pop loads such an incoming value into, by family.
  std::map<uint64_t, unsigned> EntryValueTemps;
  auto EntryValueAt = [&](const NdVar &Address) -> std::optional<unsigned> {
    if (auto Slot = SlotOf(Address))
      if (auto It = EntryValueSlots.find(
              {Slot->Family, Slot->Version, Slot->Displacement});
          It != EntryValueSlots.end())
        return It->second;
    return std::nullopt;
  };
  const std::optional<unsigned> VectorCount =
      gprFamilyOf(Img.Arch, x86reg::RAX);
  for (const size_t BI : Prologue) {
    const LowBlock &Block = F.Blocks[BI];
    TempSlots.clear();
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const LowOp &Op = Block.Ops[I];
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        break;
      for (uint8_t In = 0; In < Op.NumInputs; ++In)
        if (Op.Inputs[In].isReg() && VectorCount &&
            gprFamilyOf(Img.Arch, Op.Inputs[In].Offset) == VectorCount &&
            !((Written >> *VectorCount) & 1)) {
          ReadsVectorCount = true;
          TestsAL |=
              Op.Inputs[In].Offset == x86reg::RAX && Op.Inputs[In].Size == 1;
        }
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          Op.Inputs[1].isReg() && Op.Inputs[1].Size == TRI.PointerSize)
        if (const auto Family = gprFamilyOf(Img.Arch, Op.Inputs[1].Offset);
            Family && !((Written >> *Family) & 1))
          if (auto Address = SlotOf(Op.Inputs[0]))
            EntryValueSlots[{Address->Family, Address->Version,
                             Address->Displacement}] = *Family;
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          Op.Inputs[1].isReg() && Op.Inputs[1].Size == TRI.PointerSize)
        for (size_t K = 0; K < Registers.size(); ++K)
          if (Op.Inputs[1].Offset == Registers[K] && !Stored[K] &&
              !((Written >> (Registers[K] / 8)) & 1))
            if (auto Address = SlotOf(Op.Inputs[0]))
              Stored[K] = Spill{BI, I, *Address};
      // The lifter forms an address in a temporary, `t = base; t = t + d`,
      // and reuses the temporary in later instructions.
      if (Op.Output.isTemp()) {
        EntryValueTemps.erase(Op.Output.Offset);
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
            Op.Output.Size == TRI.PointerSize)
          if (auto Family = EntryValueAt(Op.Inputs[0]))
            EntryValueTemps[Op.Output.Offset] = *Family;
        std::optional<Slot> Address;
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
          Address = SlotOf(Op.Inputs[0]);
        else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
                 Op.NumInputs == 2 && Op.Inputs[1].isConst())
          if ((Address = SlotOf(Op.Inputs[0]))) {
            const auto Constant = static_cast<int64_t>(Op.Inputs[1].Offset);
            Address->Displacement +=
                Op.Opcode == NdOp::INT_ADD ? Constant : -Constant;
          }
        if (Address && Op.Output.Size == TRI.PointerSize)
          TempSlots[Op.Output.Offset] = *Address;
        else
          TempSlots.erase(Op.Output.Offset);
      }
      if (Op.Output.isReg())
        if (const auto Family = gprFamilyOf(Img.Arch, Op.Output.Offset)) {
          // Reloading a register's own incoming value leaves it unwritten.
          std::optional<unsigned> Reloaded;
          if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
            Reloaded = EntryValueAt(Op.Inputs[0]);
          else if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                   Op.Inputs[0].isTemp())
            if (auto It = EntryValueTemps.find(Op.Inputs[0].Offset);
                It != EntryValueTemps.end())
              Reloaded = It->second;
          if (Reloaded == *Family && Op.Output.Size == TRI.PointerSize &&
              !((Written >> *Family) & 1))
            continue;
          ++Versions[*Family];
          Written |= GPRFamilyMask(1) << *Family;
        }
    }
  }
  if (!ReadsVectorCount)
    return Spills;
  // The save area holds the variadic registers through the last one; a named
  // parameter's own home slot lies elsewhere (GCC -O0).
  size_t First = Registers.size();
  while (First > 0 && Stored[First - 1]) {
    if (First < Registers.size()) {
      const Slot &Lower = Stored[First - 1]->Address,
                 &Upper = Stored[First]->Address;
      if (Lower.Family != Upper.Family || Lower.Version != Upper.Version ||
          Lower.Displacement + TRI.PointerSize != Upper.Displacement)
        break;
    }
    --First;
  }
  for (size_t K = First; K < Registers.size(); ++K)
    Spills.insert({Stored[K]->Block, Stored[K]->Op});
  if (TestsAL && VectorSpillBlock) {
    const LowBlock &Block = F.Blocks[*VectorSpillBlock];
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const LowOp &Op = Block.Ops[I];
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2 || !Op.Inputs[1].isReg())
        continue;
      if (const auto Family = registerFamilyOf(Img.Arch, Op.Inputs[1].Offset);
          Family && Family->first >= kX64VectorFamilyBase &&
          Family->second == 0) {
        Spills.insert({*VectorSpillBlock, I});
        SavesVectors = true;
      }
    }
  }
  return Spills;
}

/// The byte and word writes in \p Block that the lifter spells as a masked
/// merge inside one instruction, `t = R & ~low; u = t | x; R = u` (`setbe
/// r8b` keeps R8[63:8]).  The old R reaches only the bytes the merge keeps,
/// so the AND is no read of R and the copy writes only R's low bytes, as a
/// direct byte write does.  \p Writes maps the copy's op index to the bytes
/// it writes; \p Reads gets the AND's.
void findMaskedMerges(const LowBlock &Block, std::map<size_t, uint8_t> &Writes,
                      std::set<size_t> &Reads) {
  auto Same = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
  };
  for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries) {
    const size_t First = Boundary.FirstOp;
    const size_t End =
        std::min<size_t>(First + Boundary.OpCount, Block.Ops.size());
    for (size_t I = First; I < End; ++I) {
      const LowOp &And = Block.Ops[I];
      if (And.Opcode != NdOp::INT_AND || And.NumInputs != 2 ||
          !And.Output.isTemp() || !And.Inputs[0].isReg() ||
          !And.Inputs[1].isConst() || And.Inputs[0].Offset % 8 != 0)
        continue;
      const NdVar &Reg = And.Inputs[0];
      const unsigned Width = Reg.Size;
      if (Width < 2 || Width > 8 || And.Output.Size != Width)
        continue;
      const uint64_t All =
          Width == 8 ? ~uint64_t{0} : (uint64_t{1} << (Width * 8)) - 1;
      const uint64_t Mask = And.Inputs[1].Offset & All;
      unsigned Low = 0;
      while (Low < Width && ((Mask >> (Low * 8)) & 0xFF) == 0)
        ++Low;
      if (Low == 0 || Low >= Width ||
          (Mask | ((uint64_t{1} << (Low * 8)) - 1)) != All)
        continue;
      // The first OR of t, and the copy of its result back into R.
      size_t Or = End, Copy = End;
      for (size_t J = I + 1; J < End && Or == End; ++J) {
        const LowOp &Op = Block.Ops[J];
        if (Op.Opcode == NdOp::INT_OR && Op.NumInputs == 2 &&
            Op.Output.isTemp() && Op.Output.Size == Width &&
            (Same(Op.Inputs[0], And.Output) || Same(Op.Inputs[1], And.Output)))
          Or = J;
      }
      for (size_t K = Or + 1; K < End && Copy == End; ++K) {
        const LowOp &Op = Block.Ops[K];
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Same(Op.Inputs[0], Block.Ops[Or].Output) && Same(Op.Output, Reg))
          Copy = K;
      }
      if (Copy == End)
        continue;
      // Nothing else in the instruction reads t or u or writes R meanwhile.
      bool Clean = true;
      for (size_t J = First; J < End && Clean; ++J) {
        const LowOp &Op = Block.Ops[J];
        for (uint8_t In = 0; In < Op.NumInputs; ++In)
          Clean &= (J == Or || !Same(Op.Inputs[In], And.Output)) &&
                   (J == Copy || !Same(Op.Inputs[In], Block.Ops[Or].Output));
        if (J > I && J < Copy && Op.Output.isReg() &&
            Op.Output.Offset / 8 == Reg.Offset / 8)
          Clean = false;
      }
      if (!Clean)
        continue;
      Reads.insert(I);
      Writes[Copy] = static_cast<uint8_t>(Low);
    }
  }
}

/// The argument registers each Linux syscall in \p Block names without
/// reading them.  The lifter keeps all six live so that each consumer can
/// choose; a syscall whose number the block sets before it, and whose arity
/// LinuxSyscallArguments.def verifies, reads only its own arguments (`exit`
/// reads RDI), as the emitters pass them.  Maps each op of such an
/// instruction to the registers it names but does not read.
std::map<size_t, llvm::ArrayRef<uint64_t>>
unreadSyscallArguments(const BinaryImage &Img, const LowBlock &Block) {
  std::map<size_t, llvm::ArrayRef<uint64_t>> Unread;
  if (Img.Arch != Arch::X64 || !runsUnderLinux(Img))
    return Unread;
  for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries) {
    const size_t First = Boundary.FirstOp;
    const size_t End =
        std::min<size_t>(First + Boundary.OpCount, Block.Ops.size());
    const bool Syscall =
        std::any_of(Block.Ops.begin() + First, Block.Ops.begin() + End,
                    [](const LowOp &Op) {
                      return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 0 &&
                             Op.Inputs[0].isConst() &&
                             static_cast<Intrinsic>(Op.Inputs[0].Offset) ==
                                 Intrinsic::X64Syscall;
                    });
    if (!Syscall)
      continue;
    const std::optional<uint64_t> Number =
        blockRegisterConstant(Block.Ops, First, x86reg::RAX, 8);
    const std::optional<unsigned> Count =
        Number ? linuxX64SyscallArgumentCount(*Number) : std::nullopt;
    if (!Count || *Count >= std::size(kLinuxX64SyscallArgumentRegisters))
      continue;
    // The lift passes RDX:RSI and R8:R10 as one operand each, which the call
    // reads whole once it reads either half.
    const llvm::ArrayRef<uint64_t> Rest =
        llvm::ArrayRef(kLinuxX64SyscallArgumentRegisters)
            .drop_front(x64SyscallRegistersRead(*Count));
    for (size_t I = First; I < End; ++I)
      Unread[I] = Rest;
  }
  return Unread;
}

/// The scalar writes in \p Block that keep a vector register's upper lanes:
/// `t = SUBBYTES(X, k); X = CONCAT(t, low)` (`cvtsi2sd xmm0, edi` keeps
/// XMM0[127:64]), the CONCAT perhaps first into a temporary that is copied to
/// X.  The kept lanes are no read of X, and the write defines only its low k
/// bytes.  \p Writes maps the op writing X to k; \p Reads gets the
/// SUBBYTES.
void findLaneMerges(Arch A, const LowBlock &Block,
                    std::map<size_t, uint8_t> &Writes,
                    std::set<size_t> &Reads) {
  auto Same = [](const NdVar &L, const NdVar &R) {
    return L.Space == R.Space && L.Offset == R.Offset && L.Size == R.Size;
  };
  for (size_t C = 0; C < Block.Ops.size(); ++C) {
    const LowOp &Concat = Block.Ops[C];
    if (Concat.Opcode != NdOp::CONCAT || Concat.NumInputs != 2 ||
        Concat.Output.Size != 16 || !Concat.Inputs[0].isTemp())
      continue;
    const unsigned Low = Concat.Inputs[1].Size;
    // The register the merge writes: CONCAT's own output, or the register a
    // following copy moves it to.
    size_t Write = C;
    std::optional<unsigned> Family = wholeVectorFamily(A, Concat.Output);
    if (!Family && Concat.Output.isTemp())
      for (size_t K = C + 1; K < Block.Ops.size() && !Family; ++K) {
        const LowOp &Copy = Block.Ops[K];
        if (Copy.Opcode == NdOp::COPY && Copy.NumInputs == 1 &&
            Same(Copy.Inputs[0], Concat.Output)) {
          Family = wholeVectorFamily(A, Copy.Output);
          Write = K;
        }
      }
    if (!Family || Low == 0 || Low >= 16)
      continue;
    const NdVar &Target = Block.Ops[Write].Output;
    // The kept lanes: a slice of the same register at the low part's width,
    // with no write to the register between it and the merge.
    for (size_t S = C; S-- > 0;) {
      const LowOp &Op = Block.Ops[S];
      if (Op.Output.isReg() && Op.Output.Offset >= Target.Offset &&
          Op.Output.Offset < Target.Offset + 16)
        break;
      if (Op.Opcode != NdOp::SUBBYTES || !Same(Op.Output, Concat.Inputs[0]))
        continue;
      if (Op.NumInputs == 2 && Same(Op.Inputs[0], Target) &&
          Op.Inputs[1].isConst() && Op.Inputs[1].Offset == Low) {
        Reads.insert(S);
        Writes[Write] = static_cast<uint8_t>(Low);
      }
      break;
    }
  }
}

/// A value's relation to the stack pointer at function entry: unrelated, an
/// entry-relative byte offset in [Lo, Hi], or a stack address whose offset is
/// not known.
struct StackValue {
  enum Kind : uint8_t { Unrelated, Range, Unknown };
  Kind K = Unrelated;
  int64_t Lo = 0;
  int64_t Hi = 0;

  static StackValue at(int64_t Offset) { return {Range, Offset, Offset}; }
  static StackValue unknown() { return {Unknown, 0, 0}; }
  StackValue shifted(int64_t Delta) const {
    return {K, Lo + Delta, Hi + Delta};
  }
  bool operator==(const StackValue &) const = default;
};

StackValue joinStack(const StackValue &A, const StackValue &B) {
  if (A.K == StackValue::Unrelated)
    return B;
  if (B.K == StackValue::Unrelated)
    return A;
  if (A.K == StackValue::Unknown || B.K == StackValue::Unknown)
    return StackValue::unknown();
  return {StackValue::Range, std::min(A.Lo, B.Lo), std::max(A.Hi, B.Hi)};
}

/// Temporaries and whole x86-64 GPRs, keyed by (is temporary, offset).
using StackKey = std::pair<bool, uint64_t>;
using StackState = std::map<StackKey, StackValue>;

std::optional<StackKey> stackKeyOf(const NdVar &V) {
  if (V.isTemp())
    return StackKey{true, V.Offset};
  if (V.isReg() && V.Offset < kX64GPRBytes)
    return StackKey{false, V.Offset / 8 * 8};
  return std::nullopt;
}

/// Only a whole 8-byte value carries an address.
bool wholeWord(const NdVar &V) {
  return V.Size == 8 && (!V.isReg() || V.Offset % 8 == 0);
}

StackValue stackValueOf(const StackState &S, const NdVar &V) {
  auto Key = stackKeyOf(V);
  if (!Key || !wholeWord(V))
    return {};
  auto It = S.find(*Key);
  return It == S.end() ? StackValue{} : It->second;
}

/// Bound the incoming stack-argument slots \p F reads: the body reads slot K
/// when it loads from the entry stack pointer plus EntryStackBase +
/// K * SlotBytes, directly or through a tail call made at the entry stack
/// pointer.  Anything that could read those slots out of sight makes the
/// bound unknown.
void summarizeIncomingStackReads(const BinaryImage &Img, CallRegisterBodyView F,
                                 const std::map<int, size_t> &IndexOfId,
                                 const std::set<va_t> &BlockStarts,
                                 const std::map<va_t, va_t> *ArgumentSources,
                                 const libc::StackProbeIndex *StackProbes,
                                 LocalRegisterEffect &Effect) {
  if (Img.Arch != Arch::X64) {
    Effect.UnknownStackReads = true;
    return;
  }
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  const IntegerArgumentLayout Layout =
      TRI.integerArgumentLayout(Img.abiFormat());
  // Win64 reserves a home slot for each register argument just above the
  // return address; System V passes the first stack argument there.
  const bool HomeSlots = Layout.CallStackBase > 0;
  const StackKey SP{false, TRI.StackPointer};
  const size_t NumArgRegs = Layout.Registers.size();
  // The home slot of argument register K lies just above the return address.
  auto HomeSlot = [&](size_t K) {
    return static_cast<int64_t>(TRI.PointerSize + K * Layout.SlotBytes);
  };
  // Spills of an incoming argument register to its own home slot in the
  // entry block, by register position: the step that spills it.
  std::vector<std::optional<size_t>> HomeSpill(NumArgRegs);
  // Home slots a load reads back.
  std::vector<bool> HomeLoaded(NumArgRegs);
  // The lowest incoming-area offset a pointer that escapes addresses.
  std::optional<int64_t> LowestEscape;
  // A pointer at or above the home area can reach the incoming arguments.
  auto Escapes = [&](const StackValue &V) {
    const bool Reaches = V.K == StackValue::Unknown ||
                         (V.K == StackValue::Range &&
                          V.Hi >= static_cast<int64_t>(TRI.PointerSize));
    if (Reaches && V.K == StackValue::Range)
      LowestEscape = LowestEscape ? std::min(*LowestEscape, V.Lo) : V.Lo;
    return Reaches;
  };
  // Argument registers the entry block writes before each of its operations;
  // a call clobbers them all.
  std::vector<uint8_t> EntryWrittenBefore;
  {
    uint8_t Written = 0;
    for (const LowOp &Op : F.Blocks.front().Ops) {
      EntryWrittenBefore.push_back(Written);
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        Written = 0xFF;
      else if (Op.Output.isReg())
        for (size_t K = 0; K < NumArgRegs; ++K)
          if (Op.Output.Offset / 8 == Layout.Registers[K] / 8)
            Written |= uint8_t(1u << K);
    }
  }
  bool Unknown = false;
  int StackArgs = 0;
  auto Run = [&](const LowBlock &Block, StackState &S) {
    auto ControlOf = [&](size_t OpIndex) {
      for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries)
        if (OpIndex >= Boundary.FirstOp &&
            OpIndex - Boundary.FirstOp < Boundary.OpCount)
          return Boundary.Control;
      return LowInstructionControl::None;
    };
    auto Set = [&](const NdVar &V, StackValue Value) {
      auto Key = stackKeyOf(V);
      if (!Key)
        return;
      if (!wholeWord(V))
        Value = {};
      // The stack pointer always addresses the stack.
      if (*Key == SP && Value.K == StackValue::Unrelated)
        Value = StackValue::unknown();
      if (Value.K == StackValue::Unrelated)
        S.erase(*Key);
      else
        S[*Key] = Value;
    };
    auto ArgumentsEscape = [&]() {
      for (uint64_t Reg : Layout.Registers)
        if (Escapes(stackValueOf(S, NdVar::reg(Reg, 8))))
          return true;
      return false;
    };
    for (size_t I = 0; I < Block.Ops.size() && !Unknown; ++I) {
      const LowOp &Op = Block.Ops[I];
      const bool TailCall = ControlOf(I) == LowInstructionControl::TailCall;
      auto In = [&](unsigned K) {
        return K < Op.NumInputs ? stackValueOf(S, Op.Inputs[K]) : StackValue{};
      };
      auto Const = [&](unsigned K) -> std::optional<int64_t> {
        if (K < Op.NumInputs && Op.Inputs[K].isConst())
          return static_cast<int64_t>(Op.Inputs[K].Offset);
        return std::nullopt;
      };
      const bool DirectTarget = Op.NumInputs > 0 && Op.Inputs[0].isConst();
      switch (Op.Opcode) {
      case NdOp::COPY:
        Set(Op.Output, In(0));
        continue;
      case NdOp::INT_ADD: {
        const StackValue A = In(0), B = In(1);
        if (A.K == StackValue::Range && Const(1))
          Set(Op.Output, A.shifted(*Const(1)));
        else if (B.K == StackValue::Range && Const(0))
          Set(Op.Output, B.shifted(*Const(0)));
        else if (A.K != StackValue::Unrelated || B.K != StackValue::Unrelated)
          Set(Op.Output, StackValue::unknown());
        else
          Set(Op.Output, {});
        continue;
      }
      case NdOp::INT_SUB: {
        const StackValue A = In(0);
        if (A.K == StackValue::Range && Const(1))
          Set(Op.Output, A.shifted(-*Const(1)));
        else if (A.K != StackValue::Unrelated)
          Set(Op.Output, StackValue::unknown());
        else
          Set(Op.Output, {});
        continue;
      }
      case NdOp::LOAD:
      case NdOp::ATOMIC_XCHG:
      case NdOp::ATOMIC_ADD:
      case NdOp::ATOMIC_CMPXCHG:
        if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          const StackValue A = In(0);
          if (A.K == StackValue::Unknown) {
            Unknown = true;
          } else if (A.K == StackValue::Range) {
            const int64_t Last =
                A.Hi + std::max<int64_t>(Op.Output.Size, 1) - 1;
            for (size_t K = 0; K < NumArgRegs; ++K)
              if (A.Lo < HomeSlot(K) + static_cast<int64_t>(Layout.SlotBytes) &&
                  HomeSlot(K) <= Last)
                HomeLoaded[K] = true;
            if (Last >= Layout.EntryStackBase)
              StackArgs = std::max<int64_t>(
                  StackArgs,
                  static_cast<int64_t>(Layout.Registers.size()) +
                      (Last - Layout.EntryStackBase) / Layout.SlotBytes + 1);
          }
        }
        // An atomic also stores its value operands.
        for (unsigned K = 1; K < Op.NumInputs; ++K)
          Unknown |= Escapes(In(K));
        Set(Op.Output, {});
        continue;
      case NdOp::STORE: {
        // Writing the slots reads none; storing a pointer to them lets
        // other code read them.
        Unknown |= Escapes(In(1));
        const StackValue A = In(0);
        if (HomeSlots && &Block == &F.Blocks.front() &&
            A.K == StackValue::Range && A.Lo == A.Hi && Op.NumInputs >= 2)
          for (size_t K = 0; K < NumArgRegs; ++K)
            if (A.Lo == HomeSlot(K) &&
                Op.Inputs[1] == NdVar::reg(Layout.Registers[K], 8) &&
                !((EntryWrittenBefore[I] >> K) & 1))
              HomeSpill[K] = I;
        continue;
      }
      case NdOp::CALL:
      case NdOp::INDIR_CALL:
      case NdOp::BRANCH:
      case NdOp::COND_BR:
      case NdOp::INDIR_BR:
        break;
      default: {
        // An x86 floating-point memory form (`addsd 8(%rsp), %xmm1`) reads
        // its operand at the address its second input holds, as many bytes
        // as its shape says: a read bounded like a load's.
        std::optional<unsigned> FPRead;
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 2 &&
            Op.Inputs[0].isConst() && Op.Inputs[2].isConst() &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            Op.MemoryOrdering == NdMemoryOrdering::None) {
          const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
          if (Op.Inputs[0].Offset == unsigned(Id) &&
              isX86FPStateMemoryIntrinsic(Id) &&
              Id != Intrinsic::X86F16CStoreState)
            if (const unsigned Bytes = x86FPStateMemoryBytes(
                    Id, Op.Output.Size,
                    static_cast<unsigned>(Op.Inputs[2].Offset)))
              FPRead = Bytes;
        }
        if (FPRead) {
          const StackValue A = In(1);
          if (A.K == StackValue::Unknown) {
            Unknown = true;
          } else if (A.K == StackValue::Range) {
            const int64_t Last = A.Hi + static_cast<int64_t>(*FPRead) - 1;
            for (size_t K = 0; K < NumArgRegs; ++K)
              if (A.Lo < HomeSlot(K) + static_cast<int64_t>(Layout.SlotBytes) &&
                  HomeSlot(K) <= Last)
                HomeLoaded[K] = true;
            if (Last >= Layout.EntryStackBase)
              StackArgs = std::max<int64_t>(
                  StackArgs,
                  static_cast<int64_t>(Layout.Registers.size()) +
                      (Last - Layout.EntryStackBase) / Layout.SlotBytes + 1);
          }
        }
        // Arithmetic on a stack address (a flag, an alignment mask) yields
        // no address; an operation that may dereference a pointer into the
        // incoming slots is a read this summary cannot bound.
        if (Op.Opcode == NdOp::INTRINSIC)
          for (unsigned K = 0; K < Op.NumInputs; ++K)
            if (!(FPRead && K == 1))
              Unknown |= Escapes(In(K));
        Set(Op.Output, {});
        continue;
      }
      }
      // Control transfer.
      const bool LeavesFunction =
          (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR)
              ? DirectTarget && !BlockStarts.count(Op.Inputs[0].Offset)
              : Op.Opcode != NdOp::INDIR_BR || Block.Succs.empty();
      if (!LeavesFunction)
        continue;
      if (Op.Opcode == NdOp::CALL && DirectTarget && !TailCall &&
          libc::stackProbeEffect(Img, Op.Inputs[0].Offset, StackProbes) ==
              libc::StackProbeEffect::Probe)
        continue;
      Unknown |= ArgumentsEscape();
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          !TailCall) {
        // A returning callee reads its own slots, below this function's, and
        // leaves nothing in the registers it may clobber.
        for (uint64_t Family = 0; Family < kX64GPRBytes; Family += 8)
          if (!TRI.isCalleeSaveReg(Family) && StackKey{false, Family} != SP)
            S.erase(StackKey{false, Family});
        continue;
      }
      // A tail call hands this function's stack to its target: a function,
      // or an import by the stub or slot it names, whose stack arguments its
      // prototype may fix (solveCallRegisterEffects).
      const bool EntryStack =
          stackValueOf(S, NdVar::reg(TRI.StackPointer, 8)) == StackValue::at(0);
      const bool ImportTarget =
          DirectTarget && Op.Opcode != NdOp::INDIR_BR &&
          (Op.Opcode == NdOp::INDIR_CALL
               ? !importCalleeName(Img, Op.Inputs[0].Offset).empty()
               : Img.findImportAt(Op.Inputs[0].Offset) != nullptr) &&
          !importOwnRoutine(Img, ArgumentSources, Op.Inputs[0].Offset);
      const std::optional<va_t> ArgumentsFrom =
          ImportTarget ? std::nullopt
                       : pointerVariableArgumentsFrom(ArgumentSources, F.Blocks,
                                                      Block, I, Op);
      if (DirectTarget && EntryStack &&
          (ImportTarget ||
           (Op.Opcode != NdOp::INDIR_CALL && Op.Opcode != NdOp::INDIR_BR)))
        Effect.StackTailCallees.insert(Op.Inputs[0].Offset);
      else if (EntryStack && ArgumentsFrom)
        Effect.StackTailCallees.insert(*ArgumentsFrom);
      else
        Unknown = true;
    }
  };

  std::vector<std::optional<StackState>> BlockEntry(F.Blocks.size());
  std::vector<unsigned> Visits(F.Blocks.size());
  BlockEntry[0] = StackState{{SP, StackValue::at(0)}};
  std::deque<size_t> Work{0};
  auto Drain = [&]() {
    while (!Work.empty() && !Unknown) {
      const size_t BI = Work.front();
      Work.pop_front();
      StackState S = *BlockEntry[BI];
      Run(F.Blocks[BI], S);
      for (int SuccId : F.Blocks[BI].Succs) {
        auto It = IndexOfId.find(SuccId);
        if (It == IndexOfId.end())
          continue;
        const size_t Succ = It->second;
        if (!BlockEntry[Succ]) {
          BlockEntry[Succ] = S;
          Work.push_back(Succ);
          continue;
        }
        StackState &Old = *BlockEntry[Succ];
        const bool Widen = ++Visits[Succ] > limits::kMaxStackOffsetJoinVisits;
        bool Changed = false;
        for (const auto &[Key, Value] : S) {
          StackValue &Slot = Old[Key];
          StackValue Next = joinStack(Slot, Value);
          if (Next == Slot)
            continue;
          Slot = Widen ? StackValue::unknown() : Next;
          Changed = true;
        }
        if (Changed)
          Work.push_back(Succ);
      }
    }
  };
  Drain();
  // A block no edge reaches (a handler the unwinder enters) runs with a stack
  // and frame pointer this summary does not know.
  const StackKey FP{false, TRI.FramePointer};
  for (size_t BI = 0; BI < F.Blocks.size() && !Unknown; ++BI) {
    if (BlockEntry[BI])
      continue;
    BlockEntry[BI] =
        StackState{{SP, StackValue::unknown()}, {FP, StackValue::unknown()}};
    Work.push_back(BI);
    Drain();
  }
  // A variadic prologue spills the first variadic argument register and
  // every later one to their home slots and hands a pointer to the first of
  // those slots on as the va_list.  A caller passes only the variadic
  // arguments it sets, so those spills read none.  Any other spill that no
  // load reads back, with no pointer to the incoming area escaping, reads
  // nothing either: only this function could observe its home slots.
  int VariadicFrom = -1;
  if (HomeSlots && LowestEscape && *LowestEscape >= HomeSlot(1) &&
      (*LowestEscape - HomeSlot(0)) % Layout.SlotBytes == 0) {
    const size_t First =
        static_cast<size_t>((*LowestEscape - HomeSlot(0)) / Layout.SlotBytes);
    bool Spilled = First < NumArgRegs;
    for (size_t K = First; Spilled && K < NumArgRegs; ++K)
      Spilled = HomeSpill[K].has_value();
    if (Spilled)
      VariadicFrom = static_cast<int>(First);
  }
  for (size_t K = 0; K < NumArgRegs; ++K) {
    if (!HomeSpill[K])
      continue;
    const bool Reads = VariadicFrom >= 0 ? static_cast<int>(K) < VariadicFrom
                                         : Unknown || HomeLoaded[K];
    if (auto Family = gprFamilyOf(Img.Arch, Layout.Registers[K]);
        Family && !Reads)
      Effect.Blocks.front().Steps[*HomeSpill[K]].Reads[*Family] = 0;
  }
  Effect.VariadicFrom = VariadicFrom;
  Effect.UnknownStackReads |= Unknown;
  Effect.StackArgs = StackArgs;
}
// A constant SELECT consumes just one arm. Track scalar facts within a block
// using the shared LowIR evaluator, so call-graph summaries and MedIR agree
// about conditional implicit inputs. Memory and relocated addresses are never
// sampled, and calls/opaque effects invalidate register facts.
struct BlockScalarFacts {
  using Fact = std::pair<uint16_t, uint64_t>;
  std::map<size_t, unsigned> SelectedArms;
  std::map<uint64_t, Fact> Registers;
};

BlockScalarFacts blockScalarFacts(const BinaryImage &Img, const LowBlock &Block,
                                  bool KeepExit) {
  BlockScalarFacts Facts;
  if (!KeepExit &&
      std::none_of(Block.Ops.begin(), Block.Ops.end(),
                   [](const LowOp &O) { return O.Opcode == NdOp::SELECT; }))
    return Facts;
  auto &Registers = Facts.Registers;
  std::map<uint64_t, BlockScalarFacts::Fact> Temps;
  auto Value = [&](const NdVar &V) -> std::optional<uint64_t> {
    if (!V.Size || V.Size > 8)
      return std::nullopt;
    const uint64_t Mask = UINT64_MAX >> ((8 - V.Size) * 8);
    if (V.isConst()) {
      if (isAddressProvenance(V.Provenance) ||
          (V.Provenance == ConstantAddressProvenance::Unknown &&
           Img.isPotentiallyRelocatableAddress(V.Offset)))
        return std::nullopt;
      return V.Offset & Mask;
    }
    if (V.isTemp()) {
      const auto It = Temps.find(V.Offset);
      return It != Temps.end() && It->second.first == V.Size
                 ? std::optional<uint64_t>{It->second.second}
                 : std::nullopt;
    }
    if (V.isReg()) {
      auto It = Registers.upper_bound(V.Offset);
      if (It != Registers.begin()) {
        --It;
        const auto [Width, Bits] = It->second;
        const uint64_t Delta = V.Offset - It->first;
        if (V.Size <= Width && Delta <= Width - V.Size)
          return (Bits >> (Delta * 8)) & Mask;
      }
    }
    return std::nullopt;
  };
  va_t Address = InvalidVA;
  for (size_t I = 0; I < Block.Ops.size(); ++I) {
    const auto &O = Block.Ops[I];
    if (O.Addr != Address) {
      Temps.clear();
      Address = O.Addr;
    }
    std::optional<uint64_t> Result;
    if (O.MemoryOrdering == NdMemoryOrdering::None &&
        O.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (O.Opcode == NdOp::SELECT && O.NumInputs == 3 && O.Output.Size &&
          (O.Output.isReg() || O.Output.isTemp()) &&
          O.Inputs[1].Size == O.Output.Size &&
          O.Inputs[2].Size == O.Output.Size) {
        if (auto C = Value(O.Inputs[0])) {
          Facts.SelectedArms[I] = *C ? 1 : 2;
          Result = Value(O.Inputs[Facts.SelectedArms[I]]);
        }
      } else
        Result = lowScalarConstantResult(O, Value);
    }
    if (O.Opcode == NdOp::CALL || O.Opcode == NdOp::INDIR_CALL ||
        O.Opcode == NdOp::INTRINSIC)
      Registers.clear();
    if (O.Output.isReg()) {
      const auto &V = O.Output;
      for (auto It = Registers.begin(); It != Registers.end();) {
        const auto Base = It->first;
        const bool Overlap = Base >= V.Offset
                                 ? Base - V.Offset < V.Size
                                 : V.Offset - Base < It->second.first;
        It = Overlap ? Registers.erase(It) : std::next(It);
      }
      if (Result)
        Registers[V.Offset] = {V.Size, *Result};
    } else if (O.Output.isTemp()) {
      Temps.erase(O.Output.Offset);
      if (Result)
        Temps[O.Output.Offset] = {O.Output.Size, *Result};
    }
  }
  return Facts;
}

/// A constant set within every return block needs no inter-block search.
/// Inspect only complete call-free bodies, with ordinary internal edges and
/// no independent or exceptional entries. Other bodies keep unknown state.
bool canSummarizeExitConstants(const BinaryImage &Img, CallRegisterBodyView F) {
  if (Img.Arch != Arch::X64 || !F.Complete || F.UnsafeIndirectBranches ||
      F.HasIndependentRoots || F.Blocks.empty())
    return false;
  size_t Operations = 0;
  std::set<int> Blocks;
  for (const LowBlock &Block : F.Blocks) {
    if (Block.Ops.empty() || !Block.ExceptionalSuccs.empty() ||
        Block.Ops.size() > limits::kMaxCallExitConstantOps - Operations ||
        !Blocks.insert(Block.Id).second)
      return false;
    Operations += Block.Ops.size();
  }
  bool Returns = false;
  bool WritesExtraRegister = false;
  const TargetRegInfo &Registers = getTargetRegInfo(Img.Arch);
  for (const LowBlock &Block : F.Blocks) {
    const auto Last = Block.Ops.back().Opcode;
    if (Last == NdOp::RETURN) {
      if (!Block.Succs.empty())
        return false;
      Returns = true;
    } else if ((Last != NdOp::BRANCH && Last != NdOp::COND_BR) ||
               Block.Succs.empty())
      return false;
    for (int Succ : Block.Succs)
      if (!Blocks.count(Succ))
        return false;
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const NdVar &Output = Block.Ops[I].Output;
      if (Output.isReg() && Output.Size == sizeof(uint64_t) &&
          gprFamilyOf(Img.Arch, Output.Offset) &&
          Output.Offset % sizeof(uint64_t) == 0 &&
          Output.Offset != Registers.StackPointer &&
          !Registers.isReturnReg(Output.Offset))
        WritesExtraRegister = true;
      const auto Opcode = Block.Ops[I].Opcode;
      if (Opcode == NdOp::CALL || Opcode == NdOp::INDIR_CALL ||
          Opcode == NdOp::INTRINSIC || Opcode == NdOp::INDIR_BR ||
          ((Opcode == NdOp::RETURN || Opcode == NdOp::BRANCH ||
            Opcode == NdOp::COND_BR) &&
           I + 1 != Block.Ops.size()))
        return false;
    }
  }
  return Returns && WritesExtraRegister;
}

// Keep only self-contained scalar query bodies. CPUID is ordered and must run
// exactly once; source expansion will clone its multi-result definition rather
// than independently recompute any of its returned words.
std::shared_ptr<const std::vector<LowOp>>
registerLeaf(const BinaryImage &Image, CallRegisterBodyView Body) {
  if (Image.Arch != Arch::X64 || !Body.Complete ||
      Body.UnsafeIndirectBranches || Body.HasIndependentRoots ||
      Body.Blocks.size() != 1)
    return {};
  const auto &Block = Body.Blocks.front();
  if (const auto *Exception =
          Image.ExceptionMetadata.findFunction(Block.StartAddr);
      Exception && (Exception->Personality != ExceptionPersonality::None ||
                    Exception->ParseStatus != ExceptionParseStatus::Complete))
    return {};
  if (!Block.Preds.empty() || !Block.Succs.empty() ||
      !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty() ||
      Block.Ops.empty() ||
      Block.Ops.size() > limits::ir::kSourceRegisterLeafOps)
    return {};
  const auto &Return = Block.Ops.back();
  const auto *Encoding = Image.readVA(Return.Addr, 1);
  if (Return.Opcode != NdOp::RETURN || Return.NumInputs != 1 ||
      Return.Inputs[0] != NdVar::reg(x86reg::RAX, 8) || !Encoding ||
      *Encoding != 0xC3)
    return {};
  const auto &Registers = getTargetRegInfo(Image.Arch);
  auto Scalar = [&](const NdVar &Value) {
    if (!Value.Size)
      return true;
    if (Value.Size > sizeof(uint64_t))
      return false;
    if (Value.isConst())
      return Value.Provenance != ConstantAddressProvenance::Address &&
             Value.Provenance != ConstantAddressProvenance::CodeAddress &&
             Value.Provenance != ConstantAddressProvenance::DataAddress &&
             Value.Provenance != ConstantAddressProvenance::AddressFragment &&
             !Image.isPotentiallyRelocatableAddress(Value.Offset);
    return Value.isTemp() ||
           (Value.isReg() && ((gprFamilyOf(Image.Arch, Value.Offset) &&
                               Value.Offset / 8 != x86reg::RSP / 8) ||
                              Registers.isFlag(Value.Offset, Value.Size)));
  };
  bool HasQuery = false;
  for (size_t I = 0; I + 1 < Block.Ops.size(); ++I) {
    const auto &Op = Block.Ops[I];
    // CLD/STD change control state outside the projected GPR results, even
    // when this caller has no explicit read of the direction flag.
    if (Op.Output.isReg() && Op.Output.Offset == x86reg::DF)
      return {};
    if (Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !Scalar(Op.Output))
      return {};
    for (unsigned K = 0; K < Op.NumInputs; ++K)
      if (!Scalar(Op.Inputs[K]))
        return {};
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
    case NdOp::SUBBYTES:
    case NdOp::CONCAT:
    case NdOp::POPCOUNT:
    case NdOp::SELECT:
      break;
    case NdOp::INTRINSIC:
      if (Op.NumInputs != 3 || !Op.Inputs[0].isConst() ||
          Op.Inputs[0].Offset != static_cast<unsigned>(Intrinsic::Cpuid))
        return {};
      HasQuery = true;
      break;
    default:
      return {};
    }
  }
  return HasQuery ? std::make_shared<const std::vector<LowOp>>(Block.Ops)
                  : nullptr;
}

GPRExitConstants scalarExitConstants(const BinaryImage &Img,
                                     const BlockScalarFacts &Facts) {
  GPRExitConstants Constants;
  const TargetRegInfo &Registers = getTargetRegInfo(Img.Arch);
  for (const auto &[Register, Fact] : Facts.Registers) {
    const auto Family = gprFamilyOf(Img.Arch, Register);
    if (Family && Register == *Family * sizeof(uint64_t) &&
        Register != Registers.StackPointer &&
        !Registers.isReturnReg(Register) && Fact.first == sizeof(uint64_t) &&
        !Img.isPotentiallyRelocatableAddress(Fact.second))
      Constants.emplace_back(Register, Fact.second);
  }
  return Constants;
}

} // namespace

std::optional<unsigned> gprFamilyOf(Arch A, uint64_t RegOff) {
  if (A != Arch::X64 || RegOff >= kX64GPRBytes)
    return std::nullopt;
  return static_cast<unsigned>(RegOff / 8);
}

std::optional<std::pair<unsigned, unsigned>> registerFamilyOf(Arch A,
                                                              uint64_t RegOff) {
  if (const auto Family = gprFamilyOf(A, RegOff))
    return std::pair{*Family, static_cast<unsigned>(RegOff % 8)};
  if (A != Arch::X64 || RegOff < x86reg::VectorBase)
    return std::nullopt;
  const uint64_t Index =
      (RegOff - x86reg::VectorBase) / x86reg::VectorRegStride;
  const uint64_t Offset =
      (RegOff - x86reg::VectorBase) % x86reg::VectorRegStride;
  if (Index >= kX64VectorArgumentFamilies || Offset >= 16)
    return std::nullopt;
  return std::pair{kX64VectorFamilyBase + static_cast<unsigned>(Index),
                   static_cast<unsigned>(Offset)};
}

bool callPreservesRegisterBytes(Arch A, uint32_t Preserved, uint64_t RegOff,
                                uint16_t Size) {
  if (const auto Family = gprFamilyOf(A, RegOff))
    return (Preserved >> *Family) & 1;
  const auto Family = registerFamilyOf(A, RegOff);
  return Family && Family->first >= kX64VectorFamilyBase &&
         ((Preserved >> Family->first) & 1) && Family->second + Size <= 16;
}

namespace {
/// Whether \p Step only reads and writes registers: it names no routine,
/// leaves for nowhere and copies no vector register.
bool isPlainStep(const RegisterStep &Step) {
  return Step.Callee == InvalidVA && !Step.TailCallee &&
         !Step.UnknownTailCall && !Step.VectorCopy && !Step.UnknownCall &&
         Step.ImportCallee == InvalidVA && Step.ArgumentsFrom == InvalidVA &&
         !Step.Exits && !Step.FixedArguments && !Step.FixedTailCall;
}
} // namespace

/// Join each run of plain steps into one step with the same effect on the
/// bytes live before it.  Backward, a plain step ends the life of the
/// families it kills and of a read no wider than its low write there, then
/// makes each family live as wide as it reads it.  For one family, with \c T
/// its low-write width (the whole family where killed), that is
/// \c max(R, w > T ? w : 0) of the width \c w live after it.  Two such steps,
/// \c First then \c Second, are the one step whose \c T is the larger of
/// theirs and whose \c R is \c First's read joined with \c Second's where
/// \c First does not satisfy it.  entryLiveWidths reads plain steps only so,
/// and every other reader of a summary looks only at steps that are not
/// plain, so the summary answers as before with a step per run instead of
/// one per instruction.
void joinPlainSteps(LocalRegisterEffect &Effect) {
  for (RegisterBlock &Block : Effect.Blocks) {
    std::vector<RegisterStep> &Steps = Block.Steps;
    size_t Kept = 0;
    for (size_t I = 0; I < Steps.size(); ++I) {
      if (Kept && isPlainStep(Steps[Kept - 1]) && isPlainStep(Steps[I])) {
        RegisterStep &First = Steps[Kept - 1];
        const RegisterStep &Second = Steps[I];
        for (size_t Family = 0; Family < First.Reads.size(); ++Family) {
          const bool Killed = (First.Kills >> Family) & 1;
          if (!Killed && Second.Reads[Family] > First.LowWrites[Family])
            First.Reads[Family] =
                std::max(First.Reads[Family], Second.Reads[Family]);
          First.LowWrites[Family] =
              std::max(First.LowWrites[Family], Second.LowWrites[Family]);
        }
        First.Kills |= Second.Kills;
        continue;
      }
      if (Kept != I)
        Steps[Kept] = std::move(Steps[I]);
      ++Kept;
    }
    // A summary is kept across runs: release the steps it no longer has.
    Steps.resize(Kept);
    Steps.shrink_to_fit();
  }
}

LocalRegisterEffect
localRegisterEffect(const BinaryImage &Img, const LowFunc &F,
                    const libc::NoReturnTargetIndex *NoReturnTargets,
                    const std::map<va_t, GPRReadWidths> *FixedCallReads,
                    const std::map<va_t, va_t> *ArgumentSources,
                    const libc::StackProbeIndex *StackProbes) {
  return localRegisterEffect(Img, CallRegisterBodyView(F), NoReturnTargets,
                             FixedCallReads, ArgumentSources, StackProbes);
}

LocalRegisterEffect
localRegisterEffect(const BinaryImage &Img, CallRegisterBodyView F,
                    const libc::NoReturnTargetIndex *NoReturnTargets,
                    const std::map<va_t, GPRReadWidths> *FixedCallReads,
                    const std::map<va_t, va_t> *ArgumentSources,
                    const libc::StackProbeIndex *StackProbes) {
  LocalRegisterEffect Effect;
  Effect.UnknownObservers = F.HasIndependentRoots;
  if (!F.Complete || F.UnsafeIndirectBranches || F.Blocks.empty()) {
    Effect.Unknown = true;
    Effect.Incomplete = true;
    Effect.UnknownStackReads = true;
    // The body's own effect is unknown, but the functions it calls directly
    // still have summaries of their own, which its call sites pass on.
    for (const LowBlock &Block : F.Blocks)
      for (const LowOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs > 0 &&
            Op.Inputs[0].isConst() && !Img.findImportAt(Op.Inputs[0].Offset))
          Effect.Callees.insert(Op.Inputs[0].Offset);
    return Effect;
  }
  std::map<int, size_t> IndexOfId;
  std::set<va_t> BlockStarts;
  for (size_t I = 0; I < F.Blocks.size(); ++I) {
    Effect.UnknownObservers |= IndexOfId.count(F.Blocks[I].Id) != 0;
    IndexOfId[F.Blocks[I].Id] = I;
    BlockStarts.insert(F.Blocks[I].StartAddr);
  }
  Effect.RegisterLeaf = registerLeaf(Img, F);
  Effect.Blocks.resize(F.Blocks.size());
  const bool SummarizeExit = canSummarizeExitConstants(Img, F);
  bool FirstReturn = true;
  const uint64_t FPReturn = getTargetRegInfo(Img.Arch).FPReturnReg;
  auto WritesFPReturn = [&](const NdVar &Out) {
    return FPReturn != 0 && Out.isReg() && Out.Size != 0 &&
           Out.Offset < FPReturn + 16 && FPReturn < Out.Offset + Out.Size;
  };
  const std::set<std::pair<size_t, size_t>> SaveAreaSpills =
      findRegisterSaveAreaSpills(Img, F, IndexOfId,
                                 Effect.SavesVectorArguments);
  // System V's AL: how many vector registers a call passes a variadic callee
  // that saves them, where the call's own setup loads a constant there.
  const size_t VectorArguments =
      Img.Arch == Arch::X64 && Img.abiFormat() != BinaryFormat::COFF
          ? getTargetRegInfo(Img.Arch).floatingParamRegs(Img.abiFormat()).size()
          : 0;
  auto VectorCountAt = [&](const LowBlock &Block, size_t Call) -> int8_t {
    if (const std::optional<uint8_t> Count =
            VectorArguments
                ? lowByteConstantAtCall(Block.Ops, Call, x86reg::RAX)
                : std::nullopt;
        Count && *Count <= VectorArguments)
      return static_cast<int8_t>(*Count);
    return -1;
  };
  for (size_t BI = 0; BI < F.Blocks.size(); ++BI) {
    const LowBlock &Block = F.Blocks[BI];
    Effect.UnknownObservers |= !Block.ExceptionalSuccs.empty();
    RegisterBlock &Out = Effect.Blocks[BI];
    for (int Succ : Block.Succs)
      if (auto It = IndexOfId.find(Succ); It != IndexOfId.end())
        Out.Succs.push_back(It->second);
      else
        Effect.UnknownObservers = true;
    std::map<size_t, uint8_t> MergeWrites;
    std::set<size_t> MergeReads;
    findMaskedMerges(Block, MergeWrites, MergeReads);
    findLaneMerges(Img.Arch, Block, MergeWrites, MergeReads);
    const std::set<size_t> OverwrittenCopies =
        overwrittenRegisterCopies(Img.Arch, Block, MergeWrites);
    const std::map<size_t, llvm::ArrayRef<uint64_t>> UnreadArguments =
        unreadSyscallArguments(Img, Block);
    const bool AtReturn =
        SummarizeExit && Block.Ops.back().Opcode == NdOp::RETURN;
    const auto ScalarFacts = blockScalarFacts(Img, Block, AtReturn);
    const auto &SelectedArms = ScalarFacts.SelectedArms;
    if (AtReturn) {
      const auto Constants = scalarExitConstants(Img, ScalarFacts);
      if (FirstReturn) {
        Effect.ExitConstants = Constants;
        FirstReturn = false;
      } else
        llvm::erase_if(Effect.ExitConstants, [&](const auto &Value) {
          return !llvm::is_contained(Constants, Value);
        });
    }
    auto ControlOf = [&](size_t OpIndex) {
      for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries)
        if (OpIndex >= Boundary.FirstOp &&
            OpIndex - Boundary.FirstOp < Boundary.OpCount)
          return std::pair{Boundary.Control, Boundary.ControlFlags};
      return std::pair{LowInstructionControl::None,
                       LowInstructionControlFlag::None};
    };
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const LowOp &Op = Block.Ops[I];
      if (const auto Arm = SelectedArms.find(I);
          Arm != SelectedArms.end() && Op.Output.isReg() &&
          Op.Output == Op.Inputs[Arm->second])
        continue; // A proven identity neither reads nor overwrites the GPR.
      const auto [Control, Flags] = ControlOf(I);
      const bool TailCall = Control == LowInstructionControl::TailCall;
      const bool DirectTarget = Op.NumInputs > 0 && Op.Inputs[0].isConst();
      const bool Transfers = Op.Opcode == NdOp::CALL ||
                             Op.Opcode == NdOp::BRANCH ||
                             Op.Opcode == NdOp::COND_BR;
      const bool EntersNoReturnFunction =
          Transfers && DirectTarget &&
          libc::isNoReturnTarget(Img, Op.Inputs[0].Offset, NoReturnTargets);
      // The CFG flags a call that never returns; the LowIR contract keeps a
      // tail call unflagged, so a tail call into a no-return function is
      // recognized here.
      const bool NoReturnCall =
          Op.Opcode == NdOp::CALL &&
          (hasLowInstructionControlFlag(Flags,
                                        LowInstructionControlFlag::NoReturn) ||
           (TailCall && EntersNoReturnFunction));
      // A stack probe changes no register its caller can observe.
      if (Op.Opcode == NdOp::CALL && DirectTarget && !TailCall &&
          libc::stackProbeEffect(Img, Op.Inputs[0].Offset, StackProbes) ==
              libc::StackProbeEffect::Probe)
        continue;
      // A vector register widened from its own low bytes (`vzeroupper`, or
      // a VEX write clearing the upper lanes) keeps those bytes in place:
      // it reads them no more than a RETURN that hands them back, and
      // defines none of them.
      if (Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs == 1 &&
          Op.Output.isReg() && Op.Inputs[0].isReg() &&
          Op.Output.Offset == Op.Inputs[0].Offset &&
          Op.Output.Size > Op.Inputs[0].Size)
        if (const auto Family = registerFamilyOf(Img.Arch, Op.Output.Offset);
            Family && Family->first >= kX64VectorFamilyBase)
          continue;
      RegisterStep Step;
      // A slice of a vector register reads that slice's bytes only, and a
      // whole copy reads the source as wide as the copy is read.
      const std::optional<unsigned> SlicedVector =
          Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
                  Op.Inputs[1].isConst()
              ? wholeVectorFamily(Img.Arch, Op.Inputs[0])
              : std::nullopt;
      const std::optional<unsigned> CopiedVector =
          Op.Opcode == NdOp::COPY && Op.NumInputs == 1
              ? wholeVectorFamily(Img.Arch, Op.Inputs[0])
              : std::nullopt;
      const std::optional<unsigned> CopyTarget =
          CopiedVector ? wholeVectorFamily(Img.Arch, Op.Output) : std::nullopt;
      if (CopiedVector && CopyTarget && !MergeWrites.count(I))
        Step.VectorCopy = {static_cast<uint8_t>(*CopyTarget),
                           static_cast<uint8_t>(*CopiedVector)};
      // A RETURN hands its caller the return register, which is no
      // argument: a function that leaves RAX alone does not read it.
      for (uint8_t In = 0; In < Op.NumInputs && Op.Opcode != NdOp::RETURN;
           ++In) {
        if (!Op.Inputs[In].isReg() || (In == 0 && MergeReads.count(I)) ||
            (In == 1 && SaveAreaSpills.count({BI, I})) ||
            (In == 0 && (Step.VectorCopy || OverwrittenCopies.count(I))))
          continue;
        if (const auto Unread = UnreadArguments.find(I);
            Unread != UnreadArguments.end() &&
            llvm::any_of(Unread->second, [&](uint64_t Register) {
              return Register / 8 == Op.Inputs[In].Offset / 8;
            }))
          continue;
        if (const auto Arm = SelectedArms.find(I);
            Arm != SelectedArms.end() && In != 0 && In != Arm->second)
          continue;
        // A zero idiom (`pxor xmm0, xmm0`) or an absorbing constant reads
        // nothing of the register, as LowToMed folds it.
        if (isLowSelfXor(Op) || lowAbsorbingConstant(Op))
          continue;
        if (In == 0 && SlicedVector)
          addRead(Img.Arch, Step.Reads,
                  Op.Inputs[0].Offset + Op.Inputs[1].Offset, Op.Output.Size);
        else
          addRead(Img.Arch, Step.Reads, Op.Inputs[In].Offset,
                  Op.Inputs[In].Size);
      }
      // The result of a call that never returns reaches no one.
      if (Op.Output.isReg() && !NoReturnCall) {
        const GPRFamilyMask Bit = familyBit(Img.Arch, Op.Output.Offset);
        Effect.Writes |= Bit;
        if (WritesFPReturn(Op.Output))
          Effect.Writes |= kFPReturnWriteBit;
        // A 32- or 64-bit write defines the whole register; a byte or word
        // write keeps the rest of the old value, which stays live exactly as
        // wide as a later read needs it.
        const auto VectorOut = registerFamilyOf(Img.Arch, Op.Output.Offset);
        const bool Vector =
            VectorOut && VectorOut->first >= kX64VectorFamilyBase;
        if (auto Merge = MergeWrites.find(I); Merge != MergeWrites.end()) {
          if (auto Family = registerFamilyOf(Img.Arch, Op.Output.Offset))
            Step.LowWrites[Family->first] =
                std::max(Step.LowWrites[Family->first], Merge->second);
        } else if (Vector) {
          // A vector register is defined whole only by a write of all its
          // bytes; a narrower one at its start keeps the rest.
          if (VectorOut->second == 0 && Op.Output.Size >= 16)
            Step.Kills |= Bit;
          else if (VectorOut->second == 0)
            Step.LowWrites[VectorOut->first] =
                std::max(Step.LowWrites[VectorOut->first],
                         static_cast<uint8_t>(Op.Output.Size));
        } else if (Op.Output.Size >= 4)
          Step.Kills |= Bit;
        else if (auto Family = gprFamilyOf(Img.Arch, Op.Output.Offset);
                 Family && Op.Output.Offset % 8 == 0)
          Step.LowWrites[*Family] = std::max<uint8_t>(
              Step.LowWrites[*Family], static_cast<uint8_t>(Op.Output.Size));
      }
      // A call whose arguments a contract at this site fixes reads exactly
      // those; it calls no function a summary describes.
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          FixedCallReads)
        if (const auto Site = FixedCallReads->find(Op.Addr);
            Site != FixedCallReads->end()) {
          Step.FixedArguments = Site->second;
          Step.FixedTailCall = TailCall;
          Step.Exits = NoReturnCall;
          Effect.CallsFixedContract = true;
          Out.Steps.push_back(Step);
          continue;
        }
      switch (Op.Opcode) {
      case NdOp::INDIR_CALL:
        // A rewritten indirect tail jump (`jmp [iat]` in an import thunk)
        // passes this function's registers on to its unknown target, unless
        // the slot is an import's and its prototype says what that reads.
        Effect.Unknown = true;
        (TailCall ? Step.UnknownTailCall : Step.UnknownCall) = true;
        if (DirectTarget &&
            !importCalleeName(Img, Op.Inputs[0].Offset).empty() &&
            !importOwnRoutine(Img, ArgumentSources, Op.Inputs[0].Offset))
          Step.ImportCallee = Op.Inputs[0].Offset;
        else if (const std::optional<va_t> Source =
                     pointerVariableArgumentsFrom(ArgumentSources, F.Blocks,
                                                  Block, I, Op))
          Step.ArgumentsFrom = *Source;
        break;
      case NdOp::INDIR_BR:
        // A resolved jump table has successors; an indirect tail jump leaves
        // for code this summary cannot see.
        if (Block.Succs.empty()) {
          Effect.Unknown = true;
          Step.UnknownTailCall = true;
        }
        break;
      case NdOp::BRANCH:
      case NdOp::COND_BR:
        // A direct branch to another function's entry leaves this body the
        // way a tail call does; the CFG keeps no block for it.
        if (DirectTarget && !BlockStarts.count(Op.Inputs[0].Offset)) {
          // A jump into a function that never returns adds no write a caller
          // can observe, and an unconditional one ends this path.
          if (!EntersNoReturnFunction)
            Effect.Callees.insert(Op.Inputs[0].Offset);
          else if (Img.userNoReturnAt(Op.Inputs[0].Offset))
            Effect.NoReturnCallees.insert(Op.Inputs[0].Offset);
          Step.Callee = Op.Inputs[0].Offset;
          Step.TailCallee = true;
          Step.VectorCount = VectorCountAt(Block, I);
          Step.Exits = EntersNoReturnFunction && Op.Opcode == NdOp::BRANCH;
        }
        break;
      case NdOp::CALL: {
        // A call that never returns cannot change a register its caller
        // reads, but it still receives this function's registers.  A stub
        // of an import the loader starts at this image's own routine calls
        // that routine.
        Step.Exits = NoReturnCall;
        const std::optional<va_t> OwnRoutine =
            DirectTarget
                ? importOwnRoutine(Img, ArgumentSources, Op.Inputs[0].Offset)
                : std::nullopt;
        if (!DirectTarget ||
            (!OwnRoutine && Img.findImportAt(Op.Inputs[0].Offset))) {
          if (!NoReturnCall)
            Effect.Unknown = true;
          (TailCall ? Step.UnknownTailCall : Step.UnknownCall) = true;
          if (DirectTarget)
            Step.ImportCallee = Op.Inputs[0].Offset;
        } else {
          const va_t Callee = OwnRoutine ? *OwnRoutine : Op.Inputs[0].Offset;
          Step.Callee = Callee;
          Step.TailCallee = TailCall;
          Step.VectorCount = VectorCountAt(Block, I);
          if (!NoReturnCall)
            Effect.Callees.insert(Callee);
          else if (!EntersNoReturnFunction || Img.userNoReturnAt(Callee))
            Effect.NoReturnCallees.insert(Callee);
        }
        break;
      }
      default:
        break;
      }
      Effect.UnknownEntryReads |= Step.UnknownTailCall &&
                                  Step.ImportCallee == InvalidVA &&
                                  Step.ArgumentsFrom == InvalidVA;
      Out.Steps.push_back(Step);
    }
  }
  summarizeIncomingStackReads(Img, F, IndexOfId, BlockStarts, ArgumentSources,
                              StackProbes, Effect);
  joinPlainSteps(Effect);
  return Effect;
}

namespace {
/// Bytes of each family live on entry to \p F, given the current callee
/// summaries.  A call to one of \p VectorArgumentCallees reads the vector
/// registers its AL counts.
GPRReadWidths entryLiveWidths(const LocalRegisterEffect &F,
                              const std::map<va_t, GPRFamilyMask> &MayWrite,
                              const std::map<va_t, GPRReadWidths> &EntryReads,
                              GPRFamilyMask VolatileFamilies,
                              GPRFamilyMask ArgumentFamilies,
                              const std::set<va_t> &DispatchThunks,
                              const std::set<va_t> &VectorArgumentCallees) {
  auto Clear = [](GPRReadWidths &Live, GPRFamilyMask Mask) {
    for (size_t I = 0; I < Live.size(); ++I)
      if ((Mask >> I) & 1)
        Live[I] = 0;
  };
  auto Transfer = [&](const RegisterBlock &Block, GPRReadWidths Live) {
    for (auto It = Block.Steps.rbegin(); It != Block.Steps.rend(); ++It) {
      const RegisterStep &Step = *It;
      // An import with a prototype reads exactly its parameters, and a call
      // through a function-pointer variable reads those of the routine the
      // variable starts at.
      const va_t ReadsFrom = Step.ImportCallee != InvalidVA
                                 ? Step.ImportCallee
                                 : Step.ArgumentsFrom;
      const auto ImportReads = ReadsFrom != InvalidVA
                                   ? EntryReads.find(ReadsFrom)
                                   : EntryReads.end();
      if (Step.Exits)
        Live.fill(0);
      if (Step.FixedArguments) {
        // A call with fixed arguments reads exactly those; a tail call
        // passes nothing else on.
        if (Step.FixedTailCall)
          Live.fill(0);
        else
          Clear(Live, VolatileFamilies);
        joinReads(Live, *Step.FixedArguments);
      } else if (Step.UnknownTailCall) {
        Live.fill(0);
        if (ImportReads != EntryReads.end())
          joinReads(Live, ImportReads->second);
        else
          for (size_t I = 0; I < Live.size(); ++I)
            if ((ArgumentFamilies >> I) & 1)
              Live[I] = 8;
      } else if (Step.UnknownCall || (Step.Callee != InvalidVA &&
                                      DispatchThunks.count(Step.Callee))) {
        Clear(Live, VolatileFamilies);
        if (ImportReads != EntryReads.end())
          joinReads(Live, ImportReads->second);
      } else if (Step.Callee != InvalidVA) {
        auto W = MayWrite.find(Step.Callee);
        Clear(Live, W != MayWrite.end() ? W->second : VolatileFamilies);
        if (auto R = EntryReads.find(Step.Callee); R != EntryReads.end()) {
          joinReads(Live, R->second);
          // The callee's summary leaves out the vector registers it saves
          // for va_arg; the call passes the low double of each its AL
          // counts, a register the caller only passes on included.
          if (Step.VectorCount > 0 && VectorArgumentCallees.count(Step.Callee))
            for (int K = 0; K < Step.VectorCount; ++K) {
              uint8_t &Width = Live[kX64VectorFamilyBase + K];
              Width = std::max<uint8_t>(Width, sizeof(double));
            }
        }
      }
      // The bytes of the copy that later reads need, before the copy's
      // write ends their life.
      const uint8_t Copied = Step.VectorCopy ? Live[Step.VectorCopy->first] : 0;
      Clear(Live, Step.Kills);
      for (size_t I = 0; I < Live.size(); ++I)
        if (Step.LowWrites[I] && Live[I] <= Step.LowWrites[I])
          Live[I] = 0;
      joinReads(Live, Step.Reads);
      if (Step.VectorCopy)
        Live[Step.VectorCopy->second] =
            std::max(Live[Step.VectorCopy->second], Copied);
    }
    return Live;
  };
  std::vector<GPRReadWidths> LiveIn(F.Blocks.size(), GPRReadWidths{});
  // Where every successor follows its block, the pass from the last block
  // reads each block's successors at their fixed point already, and a second
  // pass would change nothing.
  bool Forward = true;
  for (size_t B = 0; B < F.Blocks.size() && Forward; ++B)
    for (size_t S : F.Blocks[B].Succs)
      Forward &= S > B;
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (size_t B = F.Blocks.size(); B-- > 0;) {
      GPRReadWidths LiveOut{};
      for (size_t S : F.Blocks[B].Succs)
        if (S < LiveIn.size())
          joinReads(LiveOut, LiveIn[S]);
      const GPRReadWidths In = Transfer(F.Blocks[B], LiveOut);
      if (In != LiveIn[B]) {
        LiveIn[B] = In;
        Changed = true;
      }
    }
    if (Forward)
      break;
  }
  return LiveIn.empty() ? GPRReadWidths{} : LiveIn[0];
}
} // namespace

CallRegisterSummaries
solveCallRegisterEffects(const std::map<va_t, LocalRegisterEffect> &Funcs,
                         GPRFamilyMask VolatileFamilies,
                         GPRFamilyMask ArgumentFamilies,
                         const std::set<va_t> &DispatchThunks,
                         const std::map<va_t, GPRReadWidths> &FixedEntryReads,
                         const std::map<va_t, int> &FixedEntryStackArgs) {
  LocalRegisterEffectRefs Refs;
  for (const auto &[Entry, Effect] : Funcs)
    Refs.emplace_hint(Refs.end(), Entry, &Effect);
  return solveCallRegisterEffects(Refs, VolatileFamilies, ArgumentFamilies,
                                  DispatchThunks, FixedEntryReads,
                                  FixedEntryStackArgs);
}

CallRegisterSummaries solveCallRegisterEffects(
    const LocalRegisterEffectRefs &Funcs, GPRFamilyMask VolatileFamilies,
    GPRFamilyMask ArgumentFamilies, const std::set<va_t> &DispatchThunks,
    const std::map<va_t, GPRReadWidths> &FixedEntryReads,
    const std::map<va_t, int> &FixedEntryStackArgs) {
  CallRegisterSummaries Result;
  Result.Unread = proveUnreadCallRegisters(Funcs);
  for (const auto &[Entry, Effect] : Funcs)
    if (!Effect->Incomplete && !Effect->ExitConstants.empty() &&
        !DispatchThunks.count(Entry))
      Result.ExitConstants.emplace(Entry, Effect->ExitConstants);
  // Unknown is absorbing: a function is unknown if it or any callee is.
  std::set<va_t> Unknown;
  for (const auto &[Entry, Effect] : Funcs)
    if (Effect->Unknown)
      Unknown.insert(Entry);
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const auto &[Entry, Effect] : Funcs) {
      if (Unknown.count(Entry))
        continue;
      for (va_t Callee : Effect->Callees)
        if (!Funcs.count(Callee) || Unknown.count(Callee)) {
          Unknown.insert(Entry);
          Changed = true;
          break;
        }
    }
  }

  // Least fixed point of Writes(F) = Local(F) | Writes(callees); recursion
  // converges because masks only grow.
  std::map<va_t, GPRFamilyMask> &Writes = Result.MayWrite;
  // A call with fixed arguments may change every volatile register.
  for (const auto &[Entry, Effect] : Funcs)
    if (!Unknown.count(Entry))
      Writes[Entry] =
          Effect->Writes | (Effect->CallsFixedContract ? VolatileFamilies : 0);
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (auto &[Entry, Mask] : Writes) {
      GPRFamilyMask Next = Mask;
      for (va_t Callee : Funcs.at(Entry)->Callees)
        Next |= Writes.at(Callee);
      if (Next != Mask) {
        Mask = Next;
        Changed = true;
      }
    }
  }

  // Entry reads, also a least fixed point: a callee's reads only grow a
  // caller's.  A body we do not fully know has none, and neither does one
  // that tail-calls code whose reads are unknown: that code receives this
  // function's incoming registers.
  std::set<va_t> UnknownReads;
  auto TailCallsUnknownImport = [&](const LocalRegisterEffect &Effect) {
    return llvm::any_of(Effect.Blocks, [&](const RegisterBlock &Block) {
      return llvm::any_of(Block.Steps, [&](const RegisterStep &Step) {
        return Step.UnknownTailCall &&
               ((Step.ImportCallee != InvalidVA &&
                 !FixedEntryReads.count(Step.ImportCallee)) ||
                (Step.ArgumentsFrom != InvalidVA &&
                 !FixedEntryReads.count(Step.ArgumentsFrom) &&
                 !Funcs.count(Step.ArgumentsFrom)));
      });
    });
  };
  for (const auto &[Entry, Effect] : Funcs)
    if (!FixedEntryReads.count(Entry) &&
        (Effect->Incomplete || Effect->UnknownEntryReads ||
         TailCallsUnknownImport(*Effect)))
      UnknownReads.insert(Entry);
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const auto &[Entry, Effect] : Funcs) {
      if (UnknownReads.count(Entry) || FixedEntryReads.count(Entry))
        continue;
      const bool ForwardsToUnknown =
          llvm::any_of(Effect->Blocks, [&](const RegisterBlock &Block) {
            return llvm::any_of(Block.Steps, [&](const RegisterStep &Step) {
              // A tail call through a function-pointer variable forwards to
              // the routine the variable starts at.
              if (Step.UnknownTailCall && Step.ArgumentsFrom != InvalidVA &&
                  !FixedEntryReads.count(Step.ArgumentsFrom))
                return UnknownReads.count(Step.ArgumentsFrom) != 0;
              return Step.TailCallee && !FixedEntryReads.count(Step.Callee) &&
                     (!Funcs.count(Step.Callee) ||
                      UnknownReads.count(Step.Callee));
            });
          });
      if (ForwardsToUnknown) {
        UnknownReads.insert(Entry);
        Changed = true;
      }
    }
  }
  std::map<va_t, GPRReadWidths> &Reads = Result.EntryReads;
  for (const auto &[Entry, Effect] : Funcs)
    if (!UnknownReads.count(Entry))
      Reads[Entry] = GPRReadWidths{};
  for (const auto &[Entry, Widths] : FixedEntryReads)
    Reads[Entry] = Widths;
  std::set<va_t> VectorArgumentCallees;
  for (const auto &[Entry, Effect] : Funcs)
    if (Effect->SavesVectorArguments)
      VectorArgumentCallees.insert(Entry);
  // A function's entry reads depend only on those of the routines its steps
  // name, so a change revisits just their readers.  Visiting callees before
  // their callers reaches the same least fixed point while visiting most
  // functions once.
  std::vector<va_t> Solved;
  std::map<va_t, size_t> SolvedIndex;
  for (const auto &[Entry, Widths] : Reads)
    if (!FixedEntryReads.count(Entry) && Funcs.count(Entry)) {
      SolvedIndex.emplace(Entry, Solved.size());
      Solved.push_back(Entry);
    }
  std::vector<std::vector<size_t>> Named(Solved.size()), Readers(Solved.size());
  for (size_t I = 0; I < Solved.size(); ++I) {
    for (const RegisterBlock &Block : Funcs.at(Solved[I])->Blocks)
      for (const RegisterStep &Step : Block.Steps)
        for (va_t Routine :
             {Step.Callee, Step.ImportCallee, Step.ArgumentsFrom})
          if (Routine != InvalidVA)
            if (const auto Found = SolvedIndex.find(Routine);
                Found != SolvedIndex.end())
              Named[I].push_back(Found->second);
    llvm::sort(Named[I]);
    Named[I].erase(std::unique(Named[I].begin(), Named[I].end()),
                   Named[I].end());
    for (size_t Callee : Named[I])
      Readers[Callee].push_back(I);
  }
  // Depth-first post-order puts every callee outside a cycle first.
  std::deque<size_t> Pending;
  std::vector<uint8_t> Queued(Solved.size(), 0);
  for (size_t Root = 0; Root < Solved.size(); ++Root) {
    if (Queued[Root])
      continue;
    Queued[Root] = 1;
    std::vector<std::pair<size_t, size_t>> Stack{{Root, 0}};
    while (!Stack.empty()) {
      const size_t Node = Stack.back().first;
      size_t &Next = Stack.back().second;
      if (Next < Named[Node].size()) {
        const size_t Callee = Named[Node][Next++];
        if (!Queued[Callee]) {
          Queued[Callee] = 1;
          Stack.emplace_back(Callee, 0);
        }
        continue;
      }
      Pending.push_back(Node);
      Stack.pop_back();
    }
  }
  while (!Pending.empty()) {
    const size_t I = Pending.front();
    Pending.pop_front();
    Queued[I] = 0;
    GPRReadWidths &Widths = Reads[Solved[I]];
    GPRReadWidths Next = Widths;
    joinReads(Next, entryLiveWidths(*Funcs.at(Solved[I]), Writes, Reads,
                                    VolatileFamilies, ArgumentFamilies,
                                    DispatchThunks, VectorArgumentCallees));
    if (Next == Widths)
      continue;
    Widths = Next;
    for (size_t Reader : Readers[I])
      if (!Queued[Reader]) {
        Queued[Reader] = 1;
        Pending.push_back(Reader);
      }
  }

  // Incoming stack arguments, also a least fixed point: a tail call at the
  // entry stack pointer passes them on, so the callee's reads are this
  // function's too. A tail callee without a bounded summary leaves none.
  std::set<va_t> UnknownStack;
  for (const auto &[Entry, Effect] : Funcs)
    if (Effect->UnknownStackReads && !FixedEntryStackArgs.count(Entry))
      UnknownStack.insert(Entry);
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const auto &[Entry, Effect] : Funcs) {
      if (UnknownStack.count(Entry) || FixedEntryStackArgs.count(Entry))
        continue;
      if (llvm::any_of(Effect->StackTailCallees, [&](va_t Callee) {
            return Funcs.count(Callee) ? UnknownStack.count(Callee) != 0
                                       : !FixedEntryStackArgs.count(Callee);
          })) {
        UnknownStack.insert(Entry);
        Changed = true;
      }
    }
  }
  for (const auto &[Entry, Effect] : Funcs)
    if (Effect->VariadicFrom >= 0)
      Result.VariadicFrom[Entry] = Effect->VariadicFrom;
  std::map<va_t, int> &StackArgs = Result.EntryStackArgs;
  for (const auto &[Entry, Effect] : Funcs)
    if (!UnknownStack.count(Entry))
      StackArgs[Entry] = Effect->StackArgs;
  for (const auto &[Entry, Count] : FixedEntryStackArgs)
    StackArgs[Entry] = Count;
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (auto &[Entry, Count] : StackArgs) {
      // A fixed entry contract owns stack arguments as well as register
      // reads. Its machine stub may switch stacks or forward into a
      // dispatcher whose private frame is not this source call's ABI.
      if (FixedEntryStackArgs.count(Entry))
        continue;
      auto Func = Funcs.find(Entry);
      if (Func == Funcs.end())
        continue;
      for (va_t Callee : Func->second->StackTailCallees)
        if (StackArgs.at(Callee) > Count) {
          Count = StackArgs.at(Callee);
          Changed = true;
        }
    }
  }
  return Result;
}

} // namespace neverd
