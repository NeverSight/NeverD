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

#include "neverd/ir/Limits.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

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

/// Whether \p Op is one a stack check computes with: it reads memory and
/// registers and writes registers, and has no other effect.
bool isStackCheckStep(const LowOp &Op) {
  switch (Op.Opcode) {
  case NdOp::LOAD:
    return Op.MemoryOrdering == NdMemoryOrdering::None;
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
  case NdOp::SUBBYTES:
  case NdOp::POPCOUNT:
  case NdOp::NOP:
    return true;
  default:
    return false;
  }
}

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
  if (Kind != GoWriteBarrierKind::Buffer &&
      Kind != GoWriteBarrierKind::BufferBody)
    return std::nullopt;
  if (A == Arch::X64)
    return x86reg::R11;
  if (A == Arch::AArch64)
    return a64reg::X25;
  return std::nullopt;
}

bool goWriteBarrierKeeps(Arch A, GoWriteBarrierKind Kind, uint64_t Register) {
  if (Register % 8 != 0 || goWriteBarrierResult(A, Kind) == Register)
    return false;
  // RAX to R15 but the stack pointer; X0 to X30 but the assembler's R27.
  if (A == Arch::X64)
    return Register <= x86reg::R15 && Register != x86reg::RSP;
  if (A == Arch::AArch64)
    return Register <= a64reg::X30 && Register != a64reg::X27;
  return false;
}

std::optional<FixedIntegerCallArguments>
goWriteBarrierArguments(const BinaryImage &Img, va_t Entry) {
  const GoWriteBarrier *Barrier = goWriteBarrierAt(Img, Entry);
  if (!Barrier || Barrier->Kind == GoWriteBarrierKind::Buffer)
    return std::nullopt;
  const auto Bytes =
      static_cast<uint16_t>(getTargetRegInfo(Img.Arch).PointerSize);
  FixedIntegerCallArguments Arguments;
  if (Barrier->Kind == GoWriteBarrierKind::BufferBody) {
    if (const std::optional<uint64_t> Size =
            goWriteBarrierResult(Img.Arch, Barrier->Kind))
      Arguments.Parameters.push_back(
          {SourceABICarrierKind::IntegerRegister, *Size, 0, Bytes});
    return Arguments;
  }
  for (const uint64_t Register : {Barrier->Destination, Barrier->Value})
    Arguments.Parameters.push_back(
        {SourceABICarrierKind::IntegerRegister, Register, 0, Bytes});
  return Arguments;
}

bool goAssemblyCallsPlatformCode(const BinaryImage &Img, va_t Caller,
                                 llvm::ArrayRef<LowBlock> Blocks, size_t Block,
                                 size_t Call) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  const std::optional<uint64_t> Context = goClosureContextRegister(Img.Arch);
  // x86-64's routines of this kind switch to the system stack before the
  // call (`runtime.nanotime1`, `time.now`).  With the stack pointer they
  // hold in R12 kept, the frame they return to is known again, and the
  // source frame check refuses them for the return address the call pushed
  // on the other stack, which it cannot place.  Until it follows a stack
  // switch, an x86-64 call of this kind stays Go's, which keeps less.
  if (Img.Arch != Arch::AArch64)
    return false;
  if (!Module || !Module->RegisterArguments || !Context ||
      !isGoCode(Img, Caller) || !inRanges(Module->GoAssembly, Caller) ||
      Block >= Blocks.size() || Call >= Blocks[Block].Ops.size() ||
      Blocks[Block].Ops[Call].Opcode != NdOp::INDIR_CALL ||
      Blocks[Block].Ops[Call].NumInputs < 1)
    return false;
  // The blocks behind the call, as far back as the limit, by index.
  std::map<int, size_t> IndexOf;
  for (size_t I = 0; I < Blocks.size(); ++I)
    IndexOf.emplace(Blocks[I].Id, I);
  std::map<size_t, unsigned> Depth{{Block, 0}};
  for (std::vector<size_t> Work{Block}; !Work.empty();) {
    const size_t At = Work.back();
    Work.pop_back();
    if (Depth[At] >= limits::ir::kGoIndirectCallTargetBlocks)
      continue;
    for (const int Pred : Blocks[At].Preds)
      if (const auto It = IndexOf.find(Pred); It != IndexOf.end())
        if (Depth.try_emplace(It->second, Depth[At] + 1).second)
          Work.push_back(It->second);
  }
  // Each register and temporary holds a numbered value.  A copy keeps the
  // number and any other operation gives its result its own; a load's
  // result remembers the number of the address it read.  A register no
  // operation has written since the last point that changed them all, the
  // edge of these blocks, a call or a join of blocks that disagree, holds
  // the value that point gave it.
  using Key = std::pair<VnodeSpace, uint64_t>;
  struct State {
    std::map<Key, unsigned> Known;
    unsigned Since = 0;
    bool operator==(const State &) const = default;
  };
  enum : unsigned { Held, Result, Edge, AfterCall, Join, Merged, HeldAtJoin };
  std::map<std::tuple<unsigned, size_t, size_t, Key>, unsigned> Numbers;
  std::vector<unsigned> NumberKinds;
  const auto Number = [&](unsigned Kind, size_t A, size_t B, Key K = {}) {
    const auto [It, New] =
        Numbers.try_emplace({Kind, A, B, K}, NumberKinds.size());
    if (New)
      NumberKinds.push_back(Kind);
    return It->second;
  };
  const auto IsJoinOpaque = [&](unsigned Value) {
    return NumberKinds[Value] == Merged || NumberKinds[Value] == HeldAtJoin;
  };
  const auto ValueOf = [&](const State &S, const NdVar &V) {
    const Key K{V.Space, V.Offset};
    if (const auto It = S.Known.find(K); It != S.Known.end())
      return It->second;
    return Number(NumberKinds[S.Since] == Join ? HeldAtJoin : Held,
                  S.Since, 0, K);
  };
  const uint64_t Word = getTargetRegInfo(Img.Arch).PointerSize;
  std::map<unsigned, unsigned> LoadedFrom;
  const auto Run = [&](State &S, size_t At, size_t End) {
    for (size_t I = 0; I < End; ++I) {
      const LowOp &Op = Blocks[At].Ops[I];
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        // A call may change any register, and may return the target.
        S.Known.clear();
        S.Since = Number(AfterCall, At, I);
        continue;
      }
      if (!Op.Output.isReg() && !Op.Output.isTemp())
        continue;
      const Key Out{Op.Output.Space, Op.Output.Offset};
      const bool Whole = Op.Output.Size == Word;
      // A copy, or the sum with zero an address without a displacement is.
      const bool Same =
          Whole && Op.NumInputs >= 1 && !Op.Inputs[0].isConst() &&
          Op.Inputs[0].Size == Word &&
          ((Op.Opcode == NdOp::COPY && Op.NumInputs == 1) ||
           ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
            Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
            Op.Inputs[1].Offset == 0));
      if (Same) {
        const unsigned Value = ValueOf(S, Op.Inputs[0]);
        S.Known[Out] = Value;
      } else {
        const unsigned Value = Number(Result, At, I);
        if (Whole && Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
          LoadedFrom[Value] = ValueOf(S, Op.Inputs[0]);
        S.Known[Out] = Value;
      }
    }
  };
  // What the registers hold where a block begins: what every block that
  // leads to it leaves, where they agree.
  std::map<size_t, State> Leaves;
  const auto Enters = [&](size_t At) -> std::optional<State> {
    const LowBlock &B = Blocks[At];
    State Unknown;
    Unknown.Since = Number(Edge, At, 0);
    if (B.Preds.empty() || !B.ExceptionalPreds.empty() ||
        Depth.at(At) >= limits::ir::kGoIndirectCallTargetBlocks)
      return Unknown;
    llvm::SmallVector<const State *, 4> From;
    for (const int Pred : B.Preds) {
      const auto Index = IndexOf.find(Pred);
      if (Index == IndexOf.end() || !Depth.count(Index->second))
        return Unknown;
      if (const auto It = Leaves.find(Index->second); It != Leaves.end())
        From.push_back(&It->second);
    }
    if (From.empty())
      return std::nullopt;
    State S = *From.front();
    if (llvm::all_of(From, [&](const State *P) { return *P == S; }))
      return S;
    const bool SameSince =
        llvm::all_of(From, [&](const State *P) { return P->Since == S.Since; });
    std::set<Key> Keys;
    for (const State *P : From)
      for (const auto &Item : P->Known)
        Keys.insert(Item.first);
    S.Known.clear();
    if (!SameSince)
      S.Since = Number(Join, At, 0);
    for (const Key &K : Keys) {
      const NdVar V{K.first, K.second, static_cast<uint16_t>(Word)};
      const unsigned First = ValueOf(*From.front(), V);
      if (llvm::all_of(From,
                       [&](const State *P) { return ValueOf(*P, V) == First; }))
        S.Known[K] = First;
      else if (SameSince)
        S.Known[K] = Number(Merged, At, 0, K);
    }
    return S;
  };
  // Each pass settles at least one more block or changes nothing; a value
  // changes at most twice, from unset to a number to the block's own.
  bool Changed = true;
  for (size_t Pass = 0; Changed && Pass <= 2 * Depth.size() + 1; ++Pass) {
    Changed = false;
    for (const auto &[At, Level] : Depth) {
      (void)Level;
      std::optional<State> S = Enters(At);
      if (!S)
        continue;
      Run(*S, At, Blocks[At].Ops.size());
      const auto [It, New] = Leaves.try_emplace(At, *S);
      if (!New && It->second == *S)
        continue;
      It->second = std::move(*S);
      Changed = true;
    }
  }
  if (Changed)
    return false;
  std::optional<State> S = Enters(Block);
  if (!S)
    return false;
  Run(*S, Block, Call);
  const NdVar &Target = Blocks[Block].Ops[Call].Inputs[0];
  if (Target.isConst())
    return false;
  const auto Loaded = LoadedFrom.find(ValueOf(*S, Target));
  if (Loaded == LoadedFrom.end())
    return false;
  const unsigned ContextValue = ValueOf(*S, NdVar::reg(*Context, Word));
  // A fresh number for disagreeing inputs is not evidence that a closure
  // descriptor and the context passed to the call differ on every path.
  return !IsJoinOpaque(Loaded->second) && !IsJoinOpaque(ContextValue) &&
         Loaded->second != ContextValue;
}

CallRegisterContract callRegisterContractOf(const BinaryImage &Img, va_t Caller,
                                            va_t Callee) {
  if (!isGoCode(Img, Caller))
    return CallRegisterContract::Platform;
  // A routine the pclntab does not record is none the Go toolchain built: a
  // libc routine reached through its stub follows the platform's convention
  // and keeps its callee-saved registers, as Darwin's
  // `runtime.mmap_trampoline` expects of X19, where it holds its arguments.
  // Code with a record but no frame data, as a linker trampoline into a Go
  // function is, stays under Go's.
  if (const auto &Module = Img.ExceptionMetadata.GoModule;
      Callee != InvalidVA && Module && !Module->FrameArguments.empty() &&
      !isGoCode(Img, Callee) && !goRecordsFunction(Img, Callee))
    return CallRegisterContract::Platform;
  // Compiled code of a register-ABI image follows the internal ABI, so
  // its x86-64 calls keep X15 zero.
  const bool Internal = usesGoRegisterArguments(Img, Caller);
  if (Callee != InvalidVA && goWriteBarrierAt(Img, Callee))
    return Internal ? CallRegisterContract::GoInternalWriteBarrier
                    : CallRegisterContract::GoWriteBarrier;
  return Internal ? CallRegisterContract::GoInternal
                  : CallRegisterContract::GoCode;
}

std::optional<uint64_t> goCallResultRegister(const BinaryImage &Img,
                                             CallRegisterContract Contract,
                                             va_t Callee) {
  if (!isGoWriteBarrierContract(Contract))
    return std::nullopt;
  const GoWriteBarrier *Barrier = goWriteBarrierAt(Img, Callee);
  return Barrier ? goWriteBarrierResult(Img.Arch, Barrier->Kind) : std::nullopt;
}

bool usesGoRegisterArguments(const BinaryImage &Img, va_t Entry) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  return Module && Module->RegisterArguments &&
         (Img.Arch == Arch::X64 || Img.Arch == Arch::AArch64) &&
         isGoCode(Img, Entry) && !inRanges(Module->GoAssembly, Entry);
}

llvm::ArrayRef<uint64_t> goIntegerArgumentRegisters(Arch A) {
  static constexpr uint64_t X64[] = {x86reg::RAX, x86reg::RBX, x86reg::RCX,
                                     x86reg::RDI, x86reg::RSI, x86reg::R8,
                                     x86reg::R9,  x86reg::R10, x86reg::R11};
  // arm64: R0-R15, whose first eight are AAPCS64's positions as well.
  static constexpr uint64_t AArch64[] = {
      a64reg::X0,  a64reg::X1,  a64reg::X2,  a64reg::X3,
      a64reg::X4,  a64reg::X5,  a64reg::X6,  a64reg::X7,
      a64reg::X8,  a64reg::X9,  a64reg::X10, a64reg::X11,
      a64reg::X12, a64reg::X13, a64reg::X14, a64reg::X15};
  if (A == Arch::X64)
    return X64;
  if (A == Arch::AArch64)
    return AArch64;
  return {};
}

llvm::ArrayRef<uint64_t> goFloatArgumentRegisters(Arch A) {
  static constexpr uint64_t X64[] = {
      x86reg::vectorReg(0),  x86reg::vectorReg(1),  x86reg::vectorReg(2),
      x86reg::vectorReg(3),  x86reg::vectorReg(4),  x86reg::vectorReg(5),
      x86reg::vectorReg(6),  x86reg::vectorReg(7),  x86reg::vectorReg(8),
      x86reg::vectorReg(9),  x86reg::vectorReg(10), x86reg::vectorReg(11),
      x86reg::vectorReg(12), x86reg::vectorReg(13), x86reg::vectorReg(14)};
  static constexpr uint64_t AArch64[] = {
      a64reg::V(0),  a64reg::V(1),  a64reg::V(2),  a64reg::V(3),
      a64reg::V(4),  a64reg::V(5),  a64reg::V(6),  a64reg::V(7),
      a64reg::V(8),  a64reg::V(9),  a64reg::V(10), a64reg::V(11),
      a64reg::V(12), a64reg::V(13), a64reg::V(14), a64reg::V(15)};
  if (A == Arch::X64)
    return X64;
  if (A == Arch::AArch64)
    return AArch64;
  return {};
}

std::optional<FixedIntegerCallArguments>
goEntryRegisterOperands(const BinaryImage &Img, va_t Entry,
                        const GPRReadWidths &Reads) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  // Before the internal ABI no Go code takes a register argument, so code
  // that does is assembly, though the pclntab marks none before Go 1.18.
  if (!Module || Img.Arch != Arch::X64 || !isGoCode(Img, Entry) ||
      (Module->RegisterArguments && !inRanges(Module->GoAssembly, Entry)) ||
      goWriteBarrierAt(Img, Entry) ||
      std::binary_search(Module->MorestackEntries.begin(),
                         Module->MorestackEntries.end(), Entry) ||
      goAssemblyTakesRegisterArguments(Img, Entry, Reads))
    return std::nullopt;
  if (const std::optional<uint32_t> Bytes = goFrameArgumentBytes(Img, Entry);
      Bytes && *Bytes != 0)
    return std::nullopt;
  const llvm::ArrayRef<uint64_t> Platform =
      getTargetRegInfo(Img.Arch)
          .integerArgumentLayout(Img.abiFormat())
          .Registers;
  // There a compiled function's nil check reads RAX's low byte (`testb %al,
  // (%rcx)`), which takes no argument.
  const bool StackABI = !Module->RegisterArguments;
  const auto Read = [&](uint64_t Register) {
    return Register != x86reg::RSP && Register != x86reg::RBP &&
           Reads[Register / 8] != 0 &&
           !(StackABI && Register == x86reg::RAX && Reads[Register / 8] == 1);
  };
  constexpr unsigned GeneralFamilies = 16;
  bool Own = false;
  for (unsigned Family = 0; Family < GeneralFamilies; ++Family)
    if (const uint64_t Register = uint64_t(Family) * 8;
        Read(Register) && !llvm::is_contained(Platform, Register))
      Own = true;
  if (!Own)
    return std::nullopt;
  FixedIntegerCallArguments Arguments;
  const auto Add = [&](uint64_t Register) {
    if (Read(Register) &&
        llvm::none_of(Arguments.Parameters,
                      [&](const FixedIntegerCallArguments::Parameter &P) {
                        return P.Register == Register;
                      }))
      Arguments.Parameters.push_back({SourceABICarrierKind::IntegerRegister,
                                      Register, 0, sizeof(uint64_t)});
  };
  for (const uint64_t Register : Platform)
    Add(Register);
  for (const uint64_t Register : goIntegerArgumentRegisters(Img.Arch))
    Add(Register);
  for (unsigned Family = 0; Family < GeneralFamilies; ++Family)
    Add(uint64_t(Family) * 8);
  return Arguments;
}

bool goCallEntersInternalABI(const BinaryImage &Img, va_t Caller, va_t Callee,
                             const GPRReadWidths *CalleeReads) {
  if (Callee == InvalidVA)
    return usesGoRegisterArguments(Img, Caller);
  return usesGoRegisterArguments(Img, Callee) ||
         (CalleeReads &&
          goAssemblyTakesRegisterArguments(Img, Callee, *CalleeReads));
}

bool goResultRegister(Arch A, uint64_t RegOff) {
  if (llvm::is_contained(goIntegerArgumentRegisters(A), RegOff))
    return true;
  if (A == Arch::X64)
    return RegOff >= x86reg::VectorBase &&
           RegOff < x86reg::VectorBase + 15 * x86reg::VectorRegStride &&
           (RegOff - x86reg::VectorBase) % x86reg::VectorRegStride == 0;
  if (A == Arch::AArch64)
    return RegOff >= a64reg::V0 && RegOff < a64reg::V(16) &&
           (RegOff - a64reg::V0) % (a64reg::V(1) - a64reg::V0) == 0;
  return false;
}

std::optional<uint64_t> goClosureContextRegister(Arch A) {
  if (A == Arch::X64)
    return x86reg::RDX;
  if (A == Arch::AArch64)
    return a64reg::X26;
  return std::nullopt;
}

std::optional<uint64_t> goRoutineRegister(Arch A) {
  if (A == Arch::X64)
    return x86reg::R14;
  if (A == Arch::AArch64)
    return a64reg::X28;
  return std::nullopt;
}

bool goAssemblyTakesRegisterArguments(const BinaryImage &Img, va_t Entry,
                                      const GPRReadWidths &Reads) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  if (!Module || !Module->RegisterArguments || Img.Arch != Arch::X64 ||
      !isGoCode(Img, Entry) || !inRanges(Module->GoAssembly, Entry))
    return false;
  const llvm::ArrayRef<uint64_t> Arguments =
      goIntegerArgumentRegisters(Img.Arch);
  size_t Leading = 0;
  while (Leading < Arguments.size() && Reads[Arguments[Leading] / 8])
    ++Leading;
  if (Leading == 0)
    return false;
  constexpr unsigned GeneralFamilies = 16;
  for (unsigned Family = 0; Family < GeneralFamilies; ++Family) {
    const uint64_t Register = uint64_t(Family) * 8;
    if (!Reads[Family] || Register == x86reg::RSP || Register == x86reg::RBP ||
        Register == x86reg::RDX || Register == x86reg::R14)
      continue;
    const auto Position = llvm::find(Arguments, Register);
    if (Position == Arguments.end() ||
        static_cast<size_t>(Position - Arguments.begin()) >= Leading)
      return false;
  }
  return true;
}

std::optional<std::vector<GoArgumentSpill>>
goMorestackSpills(const BinaryImage &Img, llvm::ArrayRef<LowBlock> Blocks) {
  if (std::optional<GoStackGrowth> Growth = goStackGrowth(Img, Blocks))
    return std::move(Growth->Spills);
  return std::nullopt;
}

/// Whether \p Img's stack-growth paths are modelled: every x86-64 and
/// AArch64 image, whether its Go code passes arguments in registers or on
/// the stack (goStackArgumentLayout), as amd64 Go 1.16 and arm64 Go 1.17 and
/// earlier do.
bool goGrowthPathsModeled(const BinaryImage &Img) {
  return Img.ExceptionMetadata.GoModule &&
         (Img.Arch == Arch::X64 || Img.Arch == Arch::AArch64);
}

std::optional<GoBoundsFailure> goBoundsFailureAt(const BinaryImage &Img,
                                                 va_t CallAddr, va_t Callee) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  if (!Module || Callee == InvalidVA || Callee != Module->PanicBoundsEntry ||
      (Img.Arch != Arch::X64 && Img.Arch != Arch::AArch64))
    return std::nullopt;
  const auto &Ranges = Module->PanicBounds;
  auto It = std::upper_bound(
      Ranges.begin(), Ranges.end(), CallAddr,
      [](va_t A, const GoPanicBoundsRange &R) { return A < R.Range.Begin; });
  if (It == Ranges.begin())
    return std::nullopt;
  --It;
  if (CallAddr >= It->Range.End || It->Value < 0)
    return std::nullopt;
  // abi.BoundsDecode: the value is c * numBoundsCodes + code, where c packs
  // whether each operand is a register, the index's signedness, and the two
  // register numbers (0-15) or constants (0-31).
  constexpr int NumBoundsCodes = 9;
  GoBoundsFailure Failure;
  Failure.Code = static_cast<uint8_t>(It->Value % NumBoundsCodes);
  int C = It->Value / NumBoundsCodes;
  const bool IndexIsRegister = C & 1;
  C >>= 1;
  const bool BoundIsRegister = C & 1;
  C >>= 1;
  unsigned IndexValue = 0, BoundValue = 0;
  if (IndexIsRegister) {
    Failure.Signed = C & 1;
    C >>= 1;
    IndexValue = C & 0xF;
    C >>= 4;
  } else {
    IndexValue = C & 0x1F;
    C >>= 5;
  }
  BoundValue = BoundIsRegister ? (C & 0xF) : (C & 0x1F);
  // The registers panicBounds saves: x86-64's by hardware number but SP and
  // R14, which holds g, and AArch64's R0-R15 (src/runtime/asm_amd64.s,
  // asm_arm64.s).
  const auto Operand = [&](bool IsRegister,
                           unsigned Value) -> std::optional<GoBoundsOperand> {
    if (!IsRegister)
      return GoBoundsOperand{false, Value};
    if (Img.Arch == Arch::AArch64)
      return GoBoundsOperand{true, a64reg::X0 + uint64_t(Value) * 8};
    if (Value == 4 || Value == 14)
      return std::nullopt;
    return GoBoundsOperand{true, x86reg::RAX + uint64_t(Value) * 8};
  };
  const auto Index = Operand(IndexIsRegister, IndexValue);
  const auto Bound = Operand(BoundIsRegister, BoundValue);
  if (!Index || !Bound)
    return std::nullopt;
  Failure.Index = *Index;
  Failure.Bound = *Bound;
  return Failure;
}

bool goCallResumesAfter(const BinaryImage &Img, va_t Callee) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  if (!Module)
    return false;
  if (std::binary_search(Module->ScheduleCallEntries.begin(),
                         Module->ScheduleCallEntries.end(), Callee))
    return true;
  return goGrowthPathsModeled(Img) &&
         std::binary_search(Module->MorestackEntries.begin(),
                            Module->MorestackEntries.end(), Callee) &&
         !std::binary_search(Module->ThrowingMorestackEntries.begin(),
                             Module->ThrowingMorestackEntries.end(), Callee);
}

std::map<va_t, bool> goStatedCallReturns(const BinaryImage &Img) {
  std::map<va_t, bool> Stated = Img.UserNoReturn;
  if (const auto &Module = Img.ExceptionMetadata.GoModule)
    for (const std::vector<va_t> *Entries :
         {&Module->MorestackEntries, &Module->ScheduleCallEntries})
      for (const va_t Entry : *Entries)
        if (goCallResumesAfter(Img, Entry))
          Stated.try_emplace(Entry, false);
  return Stated;
}

bool goTakesStackArguments(const BinaryImage &Img, va_t Entry) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  return Module && isGoCode(Img, Entry) &&
         (Module->StackArguments ||
          (Module->RegisterArguments && inRanges(Module->GoAssembly, Entry)));
}

bool goRecordsStackArguments(const BinaryImage &Img, va_t Entry) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  return Module && Module->StackArguments && isGoCode(Img, Entry) &&
         goFrameArgumentBytes(Img, Entry).value_or(0) != 0;
}

bool goStackABIEntryRegister(const TargetRegInfo &TRI, uint64_t Register) {
  const Arch A = TRI.TheArch;
  // The stack ABI passes floating arguments on the stack too: a vector
  // register's incoming value is what an earlier write left there, as a
  // legacy SSE write keeps ZMM0's upper lanes and `vzeroupper` keeps XMM.
  if (TRI.isVectorReg(Register))
    return true;
  llvm::SmallVector<uint64_t, 5> Kept{TRI.StackPointer};
  if (TRI.FramePointer)
    Kept.push_back(TRI.FramePointer);
  if (TRI.LinkRegister)
    Kept.push_back(TRI.LinkRegister);
  if (const std::optional<uint64_t> Context = goClosureContextRegister(A))
    Kept.push_back(*Context);
  if (A == Arch::AArch64)
    Kept.push_back(a64reg::X28);
  // A narrower view of one of them is that register (AArch64's SP is no
  // general-purpose family).
  const auto Family = gprFamilyOf(A, Register);
  return llvm::any_of(Kept, [&](uint64_t K) {
    return K == Register || (Family && gprFamilyOf(A, K) == Family);
  });
}

IntegerArgumentLayout goStackArgumentLayout(Arch A) {
  IntegerArgumentLayout Layout;
  Layout.SlotBytes = 8;
  Layout.CallStackBase = A == Arch::AArch64 ? 8 : 0;
  Layout.EntryStackBase = 8;
  return Layout;
}

IntegerArgumentLayout goRegisterArgumentLayout(Arch A) {
  IntegerArgumentLayout Layout = goStackArgumentLayout(A);
  Layout.Registers = goIntegerArgumentRegisters(A);
  return Layout;
}

std::optional<IntegerArgumentLayout>
goRegisterArgumentLayout(Arch A, llvm::ArrayRef<uint64_t> Registers) {
  const llvm::ArrayRef<uint64_t> Go = goIntegerArgumentRegisters(A);
  if (Go.empty() || Registers != Go)
    return std::nullopt;
  return goRegisterArgumentLayout(A);
}

namespace {
/// The record of the function the `pclntab` starts at \p Entry, if any.
const GoFrameArguments *goFunctionRecord(const BinaryImage &Img, va_t Entry) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  if (!Module)
    return nullptr;
  const auto &Frames = Module->FrameArguments;
  const auto It = std::lower_bound(
      Frames.begin(), Frames.end(), Entry,
      [](const GoFrameArguments &F, va_t At) { return F.Entry < At; });
  return It != Frames.end() && It->Entry == Entry ? &*It : nullptr;
}
} // namespace

std::optional<uint32_t> goFrameArgumentBytes(const BinaryImage &Img,
                                             va_t Entry) {
  const GoFrameArguments *Record = goFunctionRecord(Img, Entry);
  return Record ? Record->Bytes : std::nullopt;
}

bool goRecordsFunction(const BinaryImage &Img, va_t Entry) {
  return goFunctionRecord(Img, Entry) != nullptr;
}

bool goInsideRecordedFunction(const BinaryImage &Img, va_t Address) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  return Module && !Module->FrameArguments.empty() && isGoCode(Img, Address) &&
         !goRecordsFunction(Img, Address);
}

bool goDuffDeviceAt(const BinaryImage &Img, va_t Address) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  return Module && inRanges(Module->DuffDevices, Address);
}

bool goInlinedRoutineAt(const BinaryImage &Img, va_t Address) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  return goDuffDeviceAt(Img, Address) ||
         (Module && inRanges(Module->RegisterArgumentTransfers, Address));
}

std::optional<unsigned> goRegisterArgumentBound(const BinaryImage &Img,
                                                va_t Entry) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  if (!Module || !Module->RegisterArguments ||
      (Img.Arch != Arch::X64 && Img.Arch != Arch::AArch64))
    return std::nullopt;
  const GoFrameArguments *Record = goFunctionRecord(Img, Entry);
  if (!Record || !Record->ArgumentComponents)
    return std::nullopt;
  return *Record->ArgumentComponents;
}

bool goCallReturnsPastCaller(const BinaryImage &Img, va_t Callee) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  return Module && Img.Arch == Arch::X64 &&
         std::binary_search(Module->ReturnsPastCallerEntries.begin(),
                            Module->ReturnsPastCallerEntries.end(), Callee);
}

std::optional<FixedIntegerCallArguments>
goRegisterStubArguments(const BinaryImage &Img, va_t Entry,
                        llvm::ArrayRef<LowBlock> Blocks) {
  if ((Img.Arch != Arch::X64 && Img.Arch != Arch::AArch64) ||
      Blocks.size() != 1 || !goTakesStackArguments(Img, Entry))
    return std::nullopt;
  const IntegerArgumentLayout Layout = goStackArgumentLayout(Img.Arch);
  const std::optional<uint32_t> Bytes = goFrameArgumentBytes(Img, Entry);
  if (!Bytes || *Bytes == 0 || *Bytes % Layout.SlotBytes != 0)
    return std::nullopt;
  const std::vector<LowOp> &Ops = Blocks.front().Ops;
  if (Ops.size() < 2 || Ops.back().Opcode != NdOp::RETURN)
    return std::nullopt;
  const LowOp &Call = Ops[Ops.size() - 2];
  if (Call.Opcode != NdOp::CALL || Call.NumInputs < 1 ||
      !Call.Inputs[0].isConst() ||
      !goTakesStackArguments(Img, Call.Inputs[0].Offset))
    return std::nullopt;
  const uint64_t StackPointer = getTargetRegInfo(Img.Arch).StackPointer;
  const uint32_t Words = *Bytes / Layout.SlotBytes;
  FixedIntegerCallArguments Arguments;
  Arguments.Parameters.resize(Words);
  std::vector<bool> Stored(Words, false);
  BaseTracker Values;
  for (const LowOp &Op : llvm::ArrayRef(Ops).drop_back(2)) {
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      // Address arithmetic: every register keeps the value it came with.
      if (!Op.Output.isTemp())
        return std::nullopt;
      Values.step(Op);
      continue;
    case NdOp::NOP:
      continue;
    case NdOp::STORE:
      break;
    default:
      return std::nullopt;
    }
    if (Op.NumInputs < 2)
      return std::nullopt;
    const std::optional<BasePlus> Slot = Values.of(Op.Inputs[0]);
    const NdVar &Value = Op.Inputs[1];
    if (!Slot || Slot->Base != StackPointer || !Value.isReg() ||
        Value.Size != Layout.SlotBytes || Value.Offset == StackPointer ||
        !isGoGeneralRegister(Img.Arch, Value.Offset) ||
        Slot->Offset < Layout.EntryStackBase ||
        (Slot->Offset - Layout.EntryStackBase) % Layout.SlotBytes != 0)
      return std::nullopt;
    const int64_t Word =
        (Slot->Offset - Layout.EntryStackBase) / Layout.SlotBytes;
    if (Word >= static_cast<int64_t>(Words) || Stored[Word])
      return std::nullopt;
    Stored[Word] = true;
    Arguments.Parameters[Word] = {SourceABICarrierKind::IntegerRegister,
                                  Value.Offset, 0,
                                  static_cast<uint16_t>(Layout.SlotBytes)};
  }
  if (llvm::is_contained(Stored, false))
    return std::nullopt;
  return Arguments;
}

std::optional<GoStackGrowth> goStackGrowth(const BinaryImage &Img,
                                           llvm::ArrayRef<LowBlock> Blocks) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  if (!goGrowthPathsModeled(Img) || Blocks.empty() ||
      Blocks.front().Ops.empty())
    return std::nullopt;
  const uint64_t StackPointer = getTargetRegInfo(Img.Arch).StackPointer;
  // The entry compares the stack pointer with the goroutine's stack guard,
  // `g.stackguard0` at 16(g), and branches to the growth path.  It computes
  // nothing else, so running it again when the function restarts is the
  // same as running it once.  g is R14 from Go 1.17 and a thread-local load
  // before it, which the check need not tell apart.
  const LowBlock &Entry = Blocks.front();
  const LowOp &Check = Entry.Ops.back();
  if (Check.Opcode != NdOp::COND_BR || Check.NumInputs < 1 ||
      !Check.Inputs[0].isConst() ||
      !llvm::all_of(llvm::ArrayRef(Entry.Ops).drop_back(), isStackCheckStep))
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
  // The low bytes of a register a narrow spill stores through a temporary
  // (arm64 `strb w2, [sp, #24]`): the register and the bytes.
  std::map<uint64_t, std::pair<uint64_t, uint16_t>> Narrowed;
  bool Throws = false;
  enum class Phase { Spilling, Reloading, Restarted } At = Phase::Spilling;
  for (const LowOp &Op : Growth->Ops) {
    switch (Op.Opcode) {
    case NdOp::STORE: {
      const std::optional<BasePlus> Slot =
          Op.NumInputs >= 2 ? Values.of(Op.Inputs[0]) : std::nullopt;
      if (At != Phase::Spilling || !Slot || Slot->Base != StackPointer ||
          Slot->Offset < 8)
        return std::nullopt;
      // A floating argument is spilled as the scalar it is, the register's
      // low four or eight bytes: `movsd [rsp+8], xmm0` stores them through
      // a temporary, `fmovd F0, 8(RSP)` as D0.  Only the first eight
      // floating registers are modelled, those a summary's vector rows name
      // (kX64VectorArgumentFamilies): a path that spills a later one names
      // an argument list no call would complete.
      const auto Argument = [&](uint64_t Register) {
        return isGoGeneralRegister(Img.Arch, Register) ||
               llvm::is_contained(goFloatArgumentRegisters(Img.Arch).take_front(
                                      kX64VectorArgumentFamilies),
                                  Register);
      };
      if (Op.Inputs[1].isReg() && Argument(Op.Inputs[1].Offset)) {
        Spills.push_back(
            {Op.Inputs[1].Offset, Op.Inputs[1].Size, Slot->Offset});
      } else if (const auto Low = Op.Inputs[1].isTemp()
                                      ? Narrowed.find(Op.Inputs[1].Offset)
                                      : Narrowed.end();
                 Low != Narrowed.end() && Argument(Low->second.first)) {
        Spills.push_back({Low->second.first, Low->second.second, Slot->Offset});
      } else {
        return std::nullopt;
      }
      break;
    }
    case NdOp::SUBBYTES:
      if (At != Phase::Spilling || Op.NumInputs != 2 || !Op.Inputs[0].isReg() ||
          !Op.Inputs[1].isConst() || Op.Inputs[1].Offset != 0 ||
          !Op.Output.isTemp())
        return std::nullopt;
      Narrowed[Op.Output.Offset] = {Op.Inputs[0].Offset, Op.Output.Size};
      break;
    case NdOp::CALL:
      if (At != Phase::Spilling || Op.NumInputs < 1 ||
          !Op.Inputs[0].isConst() ||
          !std::binary_search(Module->MorestackEntries.begin(),
                              Module->MorestackEntries.end(),
                              Op.Inputs[0].Offset))
        return std::nullopt;
      Throws = std::binary_search(Module->ThrowingMorestackEntries.begin(),
                                  Module->ThrowingMorestackEntries.end(),
                                  Op.Inputs[0].Offset);
      At = Phase::Reloading;
      break;
    case NdOp::LOAD: {
      const std::optional<BasePlus> Slot =
          Op.NumInputs >= 1 ? Values.of(Op.Inputs[0]) : std::nullopt;
      if (At != Phase::Reloading || !Slot || Slot->Base != StackPointer)
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
    // Alignment padding (`nop`, `xchg ax, ax`).
    case NdOp::NOP:
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
  GoStackGrowth Result;
  Result.Spills = std::move(Spills);
  Result.GrowthStart = Growth->StartAddr;
  Result.Restarts = !Throws;
  // A frame too large for one comparison checks twice: the block the entry
  // falls into, which nothing else reaches, computes nothing either and
  // branches to the same growth block.
  const auto Second =
      llvm::find_if(Blocks.drop_front(), [&](const LowBlock &Block) {
        return Block.StartAddr == Entry.EndAddr;
      });
  if (Second != Blocks.end() && Second != Growth && !Second->Ops.empty() &&
      Second->Preds == std::vector<int>{Entry.Id}) {
    const LowOp &Again = Second->Ops.back();
    if (Again.Opcode == NdOp::COND_BR && Again.NumInputs >= 1 &&
        Again.Inputs[0].isConst() &&
        Again.Inputs[0].Offset == Check.Inputs[0].Offset &&
        llvm::all_of(llvm::ArrayRef(Second->Ops).drop_back(), isStackCheckStep))
      Result.SecondCheck = Second->StartAddr;
  }
  return Result;
}

uint16_t goContractKeptBytes(const TargetRegInfo &TRI,
                             CallRegisterContract Contract, uint64_t RegOff,
                             uint16_t Size, std::optional<uint64_t> Result) {
  // X15 holds zero in Go's internal ABI, before a call and after it.
  if (keepsGoZeroRegister(Contract) && TRI.TheArch == Arch::X64 &&
      RegOff >= x86reg::XMM15 && RegOff + Size <= x86reg::XMM15 + 16 && Size)
    return Size;
  const uint16_t Width = TRI.FullRegWidth;
  if (Size == 0 || Width == 0 || RegOff % Width + Size > Width)
    return 0;
  const uint64_t Register = RegOff - RegOff % Width;
  // The stack pointer and Go's frame pointer keep their values across any
  // call ("RSP | Stack pointer | Same", "RBP | Frame pointer | Same" in
  // abi-internal.md, likewise RSP and R29 on arm64).
  if (TRI.isFrameReg(Register))
    return Size;
  // Do not assume R14 is unchanged by an ordinary call. Runtime paths
  // such as needm can install a different g. A recognized write barrier
  // still preserves R14 under its narrower GPR contract below.
  if (!isGoWriteBarrierContract(Contract) ||
      !isGoGeneralRegister(TRI.TheArch, Register) ||
      (Result && Register == *Result) ||
      (TRI.TheArch == Arch::AArch64 && Register == a64reg::X27))
    return 0;
  return Size;
}

} // namespace neverd
