//===- InstructionFlow.cpp - What each native instruction does -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "InstructionFlow.h"

#include "SessionImpl.h"

#include "neverd/Limits.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/libc/NoReturnLoadedTarget.h"
#include "neverd/sdk/Limits.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <iterator>
#include <map>
#include <tuple>

namespace neverd::sdk {
namespace {

/// Follows the lift's full-width values, each a register's value before the
/// instruction plus a constant, through copies and constant additions to
/// where the stack pointer ends.  A call keeps the stack pointer: the callee
/// pops the return address it pushes.
StackMove stackMove(const TargetRegInfo &Registers, llvm::ArrayRef<LowOp> Ops) {
  struct Value {
    uint64_t Register = 0;
    int64_t Offset = 0;
  };
  const uint16_t Width = Registers.PointerSize;
  std::map<std::pair<VnodeSpace, uint64_t>, Value> Values;
  // Register bytes written so far: their earlier values are gone.
  llvm::SmallVector<std::pair<uint64_t, uint64_t>, 8> Written;
  const auto Overlaps = [](uint64_t Offset, uint64_t Size, uint64_t Start,
                           uint64_t End) {
    return Offset < End && Start < Offset + Size;
  };
  const auto ValueOf = [&](const NdVar &V) -> std::optional<Value> {
    if (V.Size != Width)
      return std::nullopt;
    if (auto It = Values.find({V.Space, V.Offset}); It != Values.end())
      return It->second;
    if (!V.isReg() || llvm::any_of(Written, [&](const auto &Range) {
          return Overlaps(V.Offset, V.Size, Range.first, Range.second);
        }))
      return std::nullopt;
    return Value{V.Offset, 0};
  };
  bool Moved = false;
  for (const LowOp &Op : Ops) {
    const NdVar &Out = Op.Output;
    if (!Out.Size)
      continue;
    std::optional<Value> Result;
    if (Out.Size == Width && Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
      Result = ValueOf(Op.Inputs[0]);
    } else if (Out.Size == Width &&
               (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
               Op.NumInputs == 2) {
      for (unsigned I = 0; I < 2 && !Result; ++I) {
        // `constant - value` is not an offset from the value.
        if (Op.Opcode == NdOp::INT_SUB && I != 0)
          break;
        const NdVar &C = Op.Inputs[1 - I];
        if (!C.isConst() || C.Size == 0 || C.Size > 8)
          continue;
        const int64_t Constant = llvm::SignExtend64(C.Offset, C.Size * 8);
        const auto Base = ValueOf(Op.Inputs[I]);
        if (Base && Constant > -limits::kMaxFrameSize &&
            Constant < limits::kMaxFrameSize)
          Result = Value{Base->Register, Op.Opcode == NdOp::INT_ADD
                                             ? Base->Offset + Constant
                                             : Base->Offset - Constant};
      }
    }
    for (auto It = Values.begin(); It != Values.end();)
      It = It->first.first == Out.Space &&
                   Overlaps(Out.Offset, Out.Size, It->first.second,
                            It->first.second + Width)
               ? Values.erase(It)
               : std::next(It);
    if (Out.isReg()) {
      Written.push_back({Out.Offset, Out.Offset + Out.Size});
      Moved |= Overlaps(Out.Offset, Out.Size, Registers.StackPointer,
                        Registers.StackPointer + Width);
    }
    if (Result)
      Values[{Out.Space, Out.Offset}] = *Result;
  }
  StackMove Move;
  if (!Moved)
    return Move;
  const auto Final = Values.find({VnodeSpace::REG, Registers.StackPointer});
  if (Final == Values.end()) {
    Move.Unknown = true;
    return Move;
  }
  Move.Delta = Final->second.Offset;
  if (Final->second.Register != Registers.StackPointer)
    Move.Base = Final->second.Register;
  return Move;
}

#define NEVERD_REFERENCE_KIND(Id, Name, Letter, Code)                          \
  [[maybe_unused]] constexpr llvm::StringLiteral k##Id##ReferenceKind(Name);
#include "neverd/sdk/ReferenceKinds.def"

} // namespace

llvm::SmallVector<InstructionRef, 2>
InstructionFlowScratch::dataAccesses(const BinaryImage &Img, Decoder &Dec,
                                     const DecodedInsn &DI) {
  if ((Img.Arch == Arch::X86 || Img.Arch == Arch::X64) && Dec.detailEnabled() &&
      Dec.textEnabled() && DI.Raw && DI.Raw->detail) {
    // These scalar families have no implicit memory operand. Their ordinary
    // register/immediate forms may materialize addresses, but cannot read or
    // write one. Do not generalize this to string, stack or system families.
    bool ExplicitMemoryOnly = false;
    switch (DI.Id) {
    case X86_INS_MOV:
    case X86_INS_MOVABS:
    case X86_INS_MOVSX:
    case X86_INS_MOVSXD:
    case X86_INS_MOVZX:
    case X86_INS_ADD:
    case X86_INS_ADC:
    case X86_INS_SUB:
    case X86_INS_SBB:
    case X86_INS_AND:
    case X86_INS_OR:
    case X86_INS_XOR:
    case X86_INS_CMP:
    case X86_INS_TEST:
    case X86_INS_INC:
    case X86_INS_DEC:
    case X86_INS_NEG:
    case X86_INS_NOT:
    case X86_INS_IMUL:
    case X86_INS_MUL:
    case X86_INS_IDIV:
    case X86_INS_DIV:
    case X86_INS_SHL:
    case X86_INS_SHR:
    case X86_INS_SAR:
    case X86_INS_ROL:
    case X86_INS_ROR:
    case X86_INS_RCL:
    case X86_INS_RCR:
    case X86_INS_SHLD:
    case X86_INS_SHRD:
    case X86_INS_BSWAP:
      ExplicitMemoryOnly = true;
      break;
    default:
      break;
    }
    const auto &X86 = DI.Raw->detail->x86;
    if (ExplicitMemoryOnly && X86.op_count &&
        X86.op_count <= std::size(X86.operands) &&
        llvm::all_of(llvm::ArrayRef(X86.operands, X86.op_count),
                     [](const cs_x86_op &Op) {
                       return Op.type == X86_OP_REG || Op.type == X86_OP_IMM;
                     }))
      return {};
  }
  return summarize(Img, Dec, DI).Refs;
}

/// The flow of \p DI, with its stack pointer move when \p StackRegisters is
/// given.
InstructionFlow summarizeInstructionFlow(const BinaryImage &Img, Decoder &Dec,
                                         const DecodedInsn &DI,
                                         const TargetRegInfo *StackRegisters) {
  InstructionFlowScratch Scratch;
  return Scratch.summarize(Img, Dec, DI, StackRegisters);
}

InstructionFlow
InstructionFlowScratch::summarize(const BinaryImage &Img, Decoder &Dec,
                                  const DecodedInsn &DI,
                                  const TargetRegInfo *StackRegisters) {
  InstructionFlow Flow;
  Ops.clear();
  // Each row is lifted independently of its neighbours.
  Dec.resetX86FpuState();
  try {
    // The relocations inside the instruction make its relocated immediates
    // addresses, as they are to the pipeline.
    const InstructionRelocations Relocations = instructionRelocations(Img, DI);
    Dec.liftToLow(DI, Ops, Relocations.Addresses, Relocations.Scalars);
  } catch (const UnliftedInstruction &) {
    // Nothing is known about what the instruction does, so it states no
    // transfer, no reference and no stack pointer.
    Flow.Kind = UnliftedFlow;
    Flow.Stack.Unknown = true;
    return Flow;
  }
  if (StackRegisters)
    Flow.Stack = stackMove(*StackRegisters, Ops);
  const auto ConstantInput = [](const LowOp &Op) -> std::optional<va_t> {
    if (Op.NumInputs == 0 || !Op.Inputs[0].isConst())
      return std::nullopt;
    return static_cast<va_t>(Op.Inputs[0].Offset);
  };
  // Values the instruction's own ops compute from constants, such as a
  // RIP-relative address: the next instruction's address plus a
  // displacement.  A sum is an address when one of its terms is; a
  // segment offset alone, which the lifter marks scalar, is not.
  enum class Role : uint8_t { Unknown, Scalar, Address };
  struct Known {
    va_t Value;
    Role Kind;
  };
  struct Slot {
    VnodeSpace Space;
    uint64_t Offset;
    uint16_t Size;
    Known Value;
  };
  llvm::SmallVector<Slot, 8> Computed;
  // Addresses the instruction materializes as values: exact address
  // constants (a relocated immediate, a PC-relative address) that it stores
  // or puts in a register.  A number that only looks like an address is not
  // one; without a relocation an immediate stays a number.
  llvm::SmallVector<va_t, 2> Offsets;
  const auto ValueOf = [&](const NdVar &V) -> std::optional<Known> {
    if (V.isConst())
      return Known{static_cast<va_t>(V.Offset),
                   isExactAddressProvenance(V.Provenance) ? Role::Address
                   : V.Provenance == ConstantAddressProvenance::Scalar
                       ? Role::Scalar
                       : Role::Unknown};
    for (const Slot &S : Computed)
      if (S.Space == V.Space && S.Offset == V.Offset && S.Size == V.Size)
        return S.Value;
    return std::nullopt;
  };
  const auto Record = [&](const LowOp &Op) {
    const NdVar &Out = Op.Output;
    if (!Out.Size || Out.isConst())
      return;
    std::optional<Known> Result;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
      Result = ValueOf(Op.Inputs[0]);
    } else if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2) {
      const auto Left = ValueOf(Op.Inputs[0]), Right = ValueOf(Op.Inputs[1]);
      if (Left && Right)
        Result =
            Known{Left->Value + Right->Value,
                  Left->Kind == Role::Address || Right->Kind == Role::Address
                      ? Role::Address
                  : Left->Kind == Role::Scalar || Right->Kind == Role::Scalar
                      ? Role::Scalar
                      : Role::Unknown};
    }
    llvm::erase_if(Computed, [&](const Slot &S) {
      return S.Space == Out.Space && S.Offset < Out.Offset + Out.Size &&
             Out.Offset < S.Offset + S.Size;
    });
    if (!Result)
      return;
    if (Out.Size < 8)
      Result->Value &= (uint64_t{1} << (Out.Size * 8)) - 1;
    Computed.push_back({Out.Space, Out.Offset, Out.Size, *Result});
  };
  for (const LowOp &Op : Ops) {
    switch (Op.Opcode) {
    case NdOp::CALL:
      if (auto Target = ConstantInput(Op)) {
        Flow.Kind = "call";
        Flow.Target = *Target;
        Flow.NoReturn = NoReturnTargets &&
                        libc::isNoReturnTarget(Img, *Target, NoReturnTargets);
      } else {
        Flow.Kind = "icall";
      }
      break;
    case NdOp::INDIR_CALL:
      Flow.Kind = "icall";
      if (auto Slot = ConstantInput(Op)) {
        Flow.Refs.push_back({*Slot, "read"});
        Flow.NoReturn = NoReturnTargets && libc::isNoReturnLoadedTarget(
                                               Img, *Slot, NoReturnTargets);
      }
      break;
    case NdOp::BRANCH:
    case NdOp::COND_BR:
      Flow.Kind = Op.Opcode == NdOp::BRANCH ? "jump" : "cjump";
      if (auto Target = ConstantInput(Op))
        Flow.Target = *Target;
      break;
    case NdOp::INDIR_BR:
      Flow.Kind = "ijump";
      if (auto Slot = ConstantInput(Op))
        Flow.Refs.push_back({*Slot, "read"});
      break;
    case NdOp::RETURN:
      Flow.Kind = "ret";
      break;
    case NdOp::LOAD:
    case NdOp::STORE: {
      const LowMemoryOperandView Memory = lowMemoryOperands(Op);
      if (!Memory.Complete || !Memory.Address)
        break;
      if (const auto Address = ValueOf(*Memory.Address);
          Address && Address->Kind != Role::Scalar)
        Flow.Refs.push_back({Address->Value,
                             Op.Opcode == NdOp::LOAD ? "read" : "write",
                             Memory.AccessSize});
      // A stored address, such as an argument pushed as `offset aText`.
      if (Op.Opcode == NdOp::STORE && Memory.StoredValue)
        if (const auto Stored = ValueOf(*Memory.StoredValue);
            Stored && Stored->Kind == Role::Address)
          Offsets.push_back(Stored->Value);
      break;
    }
    default:
      break;
    }
    Record(Op);
    // An address the instruction puts in a register, as `mov eax, offset X`.
    if (Op.Output.isReg())
      if (const auto Value = ValueOf(Op.Output);
          Value && Value->Kind == Role::Address)
        Offsets.push_back(Value->Value);
  }
  // A predicated call's unexecuted arm still reaches the next instruction.
  // The lift carries that guard even when its last transfer is the call.
  if (Flow.NoReturn && llvm::any_of(Ops, [](const LowOp &Op) {
        return Op.Opcode == NdOp::COND_BR;
      }))
    Flow.NoReturn = false;
  if (va_t Address = Dec.pcRelCodeRefTarget(DI); Address != InvalidVA)
    Offsets.push_back(Address);
  // The next instruction's address, which a call pushes and a system call
  // saves, is where execution returns, not an address the code refers to.
  const va_t Next = DI.Addr + DI.Size;
  for (const va_t Address : Offsets)
    if (Address != Next && Address != Flow.Target &&
        llvm::none_of(Flow.Refs, [&](const InstructionRef &Ref) {
          return Ref.To == Address && Ref.Kind == "offset";
        }))
      Flow.Refs.push_back({Address, "offset"});
  return Flow;
}

namespace {
std::vector<const FuncInfo *>
orderedFunctions(llvm::ArrayRef<FuncInfo> Functions) {
  std::vector<const FuncInfo *> Ordered;
  Ordered.reserve(Functions.size());
  for (const FuncInfo &F : Functions)
    Ordered.push_back(&F);
  const auto ByEntry = [](const FuncInfo *Left, const FuncInfo *Right) {
    return Left->Entry < Right->Entry;
  };
  if (!std::is_sorted(Ordered.begin(), Ordered.end(), ByEntry))
    std::stable_sort(Ordered.begin(), Ordered.end(), ByEntry);
  Ordered.erase(std::unique(Ordered.begin(), Ordered.end(),
                            [](const FuncInfo *Left, const FuncInfo *Right) {
                              return Left->Entry == Right->Entry;
                            }),
                Ordered.end());
  return Ordered;
}

FunctionPage pageFunctions(llvm::ArrayRef<const FuncInfo *> Ordered,
                           va_t FirstEntry, int MaxFunctions) {
  const auto First = std::lower_bound(
      Ordered.begin(), Ordered.end(), FirstEntry,
      [](const FuncInfo *F, va_t Entry) { return F->Entry < Entry; });
  const size_t End =
      std::min(static_cast<size_t>(Ordered.end() - First),
               static_cast<size_t>(std::clamp(
                   MaxFunctions, 1, FunctionPage::MaxFunctionsPerPage)));
  FunctionPage Page;
  if (End < static_cast<size_t>(Ordered.end() - First))
    Page.NextEntry = First[End]->Entry;
  Page.Functions.assign(First, First + End);
  // The order holds each entry once, ascending: every function's entry.
  Page.Entries.reserve(Ordered.size());
  for (const FuncInfo *F : Ordered)
    Page.Entries.push_back(F->Entry);
  return Page;
}
} // namespace

FunctionPage functionPage(const Session &S, va_t FirstEntry, int MaxFunctions) {
  if (!S.FunctionPages || S.FunctionPages->Revision != S.FunctionListRevision ||
      S.FunctionPages->List != S.Functions.data() ||
      S.FunctionPages->Count != S.Functions.size())
    S.FunctionPages = Session::FunctionPageIndex{
        S.FunctionListRevision, S.Functions.data(), S.Functions.size(),
        orderedFunctions(S.Functions)};
  return pageFunctions(S.FunctionPages->Ordered, FirstEntry, MaxFunctions);
}

FunctionPage functionPage(llvm::ArrayRef<FuncInfo> Functions, va_t FirstEntry,
                          int MaxFunctions) {
  return pageFunctions(orderedFunctions(Functions), FirstEntry, MaxFunctions);
}

namespace {

/// The instructions of a function without a recorded size: those its own
/// direct jumps reach from \p Entry, short of another function's entry in
/// \p Entries, in address order.  32-bit ARM, whose jumps can change the
/// instruction set, reads up to the first terminator instead.
std::vector<va_t> unsizedBody(const BinaryImage &Img, Decoder &Dec,
                              InstructionFlowScratch &Scratch, va_t Entry,
                              llvm::ArrayRef<va_t> Entries,
                              const std::function<bool()> &Stop,
                              FunctionDecodeStatus &Status) {
  const auto OtherEntry = [&](va_t Address) {
    return Address != Entry &&
           std::binary_search(Entries.begin(), Entries.end(), Address);
  };
  const bool FollowsJumps = Img.Arch != Arch::ARM && !Entries.empty();
  std::set<va_t> Body;
  std::vector<va_t> Pending{Entry};
  while (!Pending.empty() && Body.size() < kMaxUnsizedInstructions &&
         !(Stop && Stop())) {
    const va_t Start = Pending.back();
    Pending.pop_back();
    bool Terminated = false;
    const bool ModeKnown = decodeNativeRange(
        Img, Dec, Start, 0, kMaxUnsizedInstructions - Body.size(),
        [&](const DecodedInsn &DI, const uint8_t *, int) {
          if ((Stop && Stop()) || OtherEntry(DI.Addr) ||
              !Body.insert(DI.Addr).second) {
            Terminated = true;
            return false;
          }
          Terminated = Dec.isFunctionTerminator(DI);
          if (FollowsJumps) {
            const InstructionFlow Flow = Scratch.summarize(Img, Dec, DI);
            Status.Incomplete |=
                Flow.Kind == UnliftedFlow || Flow.Kind == "ijump";
            if ((Flow.Kind == "jump" || Flow.Kind == "cjump") &&
                Flow.Target != InvalidVA && !OtherEntry(Flow.Target) &&
                !Body.count(Flow.Target))
              Pending.push_back(Flow.Target);
            // A straight-line terminator can end only the current block:
            // AArch64 B.cond shares B's decode classification. The ordinary
            // lift's conditional flow still reaches the next instruction.
            if (Flow.Kind == "cjump")
              Terminated = false;
            Terminated |= Flow.NoReturn;
          }
          return !Terminated;
        });
    Status.Incomplete |= !ModeKnown || !Terminated;
  }
  Status.Incomplete |= !Pending.empty();
  Status.Stopped = Stop && Stop();
  return {Body.begin(), Body.end()};
}

/// decodeFunctions over the functions of \p Extents, each an entry and its
/// recorded size; \p Entries are every function's entries.
bool decodeExtents(
    const BinaryImage &Img, llvm::ArrayRef<std::pair<va_t, uint64_t>> Extents,
    llvm::ArrayRef<va_t> Entries,
    llvm::function_ref<void(size_t Index, Decoder &, const DecodedInsn &,
                            InstructionFlowScratch &)>
        Visit,
    const std::function<bool()> &Stop = {},
    std::vector<FunctionDecodeStatus> *Status = nullptr,
    const libc::NoReturnTargetIndex *NoReturnTargets = nullptr) {
  std::vector<FunctionDecodeStatus> Reports(Extents.size());
  // The parts of a function its unwind records keep apart from its body,
  // past other functions' code, are its code too: MSVC moves cold paths
  // there.
  std::map<va_t, std::vector<ExceptionAddressRange>> Chunks;
  for (const auto &Chunk : Img.ExceptionMetadata.outlyingChunks())
    Chunks[Chunk.Function].push_back(Chunk.Range);
  std::atomic<bool> DecoderFailed{false};
  std::optional<libc::NoReturnTargetIndex> LocalNoReturnTargets;
  if (!NoReturnTargets && llvm::any_of(Extents, [](const auto &Extent) {
        return !Extent.second;
      })) {
    LocalNoReturnTargets.emplace(Img);
    NoReturnTargets = &*LocalNoReturnTargets;
  }
  parallelForEach(Extents.size(), [&](auto Claim, size_t Total) {
    Decoder Local;
    if (!Local.init(Img)) {
      DecoderFailed = true;
      return;
    }
    InstructionFlowScratch Scratch(NoReturnTargets);
    for (size_t Index = Claim(); Index < Total && !(Stop && Stop());
         Index = Claim()) {
      const auto [Entry, Size] = Extents[Index];
      auto &Report = Reports[Index];
      const auto ReadRange = [&](va_t Start, uint64_t Span, uint64_t Limit) {
        uint64_t Bytes = 0;
        const bool ModeKnown =
            decodeNativeRange(Img, Local, Start, Span, Limit,
                              [&](const DecodedInsn &DI, const uint8_t *, int) {
                                if (Stop && Stop())
                                  return false;
                                ++Report.Instructions;
                                Report.Bytes += DI.Size;
                                Bytes += DI.Size;
                                Visit(Index, Local, DI, Scratch);
                                return true;
                              });
        Report.Incomplete |= !ModeKnown || (Span && Bytes != Span);
        Report.Stopped |= Stop && Stop();
      };
      if (Size > 0) {
        ReadRange(Entry, Size, Size);
      } else {
        for (const va_t Address :
             unsizedBody(Img, Local, Scratch, Entry, Entries, Stop, Report)) {
          if (Stop && Stop())
            break;
          ReadRange(Address, 0, 1);
        }
      }
      if (const auto It = Chunks.find(Entry); It != Chunks.end())
        for (const ExceptionAddressRange &Range : It->second) {
          if (Stop && Stop())
            break;
          ReadRange(Range.Begin, Range.End - Range.Begin,
                    Range.End - Range.Begin);
        }
    }
  });
  if (Stop && Stop())
    for (auto &Report : Reports) {
      Report.Stopped = true;
      Report.Incomplete = true;
    }
  if (Status)
    *Status = std::move(Reports);
  return !DecoderFailed;
}

/// Whether \p Ref states a width that names data (DataAccessWidths::add).
bool isDataAccess(const InstructionRef &Ref) {
  return Ref.Kind == kReadReferenceKind || Ref.Kind == kWriteReferenceKind;
}
} // namespace

bool decodeFunctions(
    const BinaryImage &Img, const FunctionPage &Page,
    llvm::function_ref<void(size_t Index, Decoder &, const DecodedInsn &,
                            InstructionFlowScratch &)>
        Visit,
    const std::function<bool()> &Stop,
    std::vector<FunctionDecodeStatus> *Status,
    const libc::NoReturnTargetIndex *NoReturnTargets) {
  std::vector<std::pair<va_t, uint64_t>> Extents;
  Extents.reserve(Page.Functions.size());
  for (const FuncInfo *F : Page.Functions)
    Extents.emplace_back(F->Entry, F->Size);
  return decodeExtents(Img, Extents, Page.Entries, Visit, Stop, Status,
                       NoReturnTargets);
}

std::optional<DataAccessWidths>
collectDataAccessWidths(const BinaryImage &Img,
                        llvm::ArrayRef<FuncInfo> Functions) {
  std::vector<std::pair<va_t, unsigned>> Accesses;
  const auto Ordered = orderedFunctions(Functions);
  std::optional<libc::NoReturnTargetIndex> NoReturnTargets;
  if (llvm::any_of(Ordered, [](const FuncInfo *F) { return !F->Size; }))
    NoReturnTargets.emplace(Img);
  for (std::optional<va_t> Cursor = va_t{0}; Cursor;) {
    const FunctionPage Page =
        pageFunctions(Ordered, *Cursor, FunctionPage::MaxFunctionsPerPage);
    // Each function's accesses, which only the thread decoding it writes.
    std::vector<std::vector<std::pair<va_t, unsigned>>> Slots(
        Page.Functions.size());
    const bool Decoded = decodeFunctions(
        Img, Page,
        [&](size_t Index, Decoder &Dec, const DecodedInsn &DI,
            InstructionFlowScratch &Scratch) {
          for (const InstructionRef &Ref : Scratch.summarize(Img, Dec, DI).Refs)
            if (isDataAccess(Ref))
              Slots[Index].emplace_back(Ref.To, Ref.Bytes);
        },
        {}, nullptr, NoReturnTargets ? &*NoReturnTargets : nullptr);
    if (!Decoded)
      return std::nullopt;
    for (const auto &Slot : Slots)
      Accesses.insert(Accesses.end(), Slot.begin(), Slot.end());
    Cursor = Page.NextEntry;
  }
  llvm::sort(Accesses);
  DataAccessWidths Widths;
  for (const auto &[Address, Width] : Accesses)
    Widths.add(Address, Width);
  return Widths;
}

std::optional<DataAccessWidths>
DataAccessScan::update(const BinaryImage &Img,
                       llvm::ArrayRef<FuncInfo> Functions) {
  // The extent collectDataAccessWidths reads each function with: the one
  // its pages give the function's entry.
  std::vector<std::pair<va_t, uint64_t>> Next;
  std::vector<va_t> NextEntries;
  const auto Ordered = orderedFunctions(Functions);
  Next.reserve(Ordered.size());
  NextEntries.reserve(Ordered.size());
  for (const FuncInfo *F : Ordered) {
    Next.emplace_back(F->Entry, F->Size);
    NextEntries.push_back(F->Entry);
  }
  // A function without a size reads up to the other functions' entries, so
  // with other entries what it read before cannot be read again to take it
  // back: read the list again whole.
  if (NextEntries != Entries)
    clear();
  // The extents read again: those read before that the list no longer
  // holds, which stop counting, and then those not read before.
  std::vector<std::pair<va_t, uint64_t>> Read;
  std::set_difference(Extents.begin(), Extents.end(), Next.begin(), Next.end(),
                      std::back_inserter(Read));
  const size_t Dropped = Read.size();
  std::set_difference(Next.begin(), Next.end(), Extents.begin(), Extents.end(),
                      std::back_inserter(Read));
  // Each extent's accesses, which only the thread decoding it writes.
  std::vector<std::vector<std::pair<va_t, unsigned>>> Slots(Read.size());
  if (!Read.empty() &&
      !decodeExtents(Img, Read, NextEntries,
                     [&](size_t Index, Decoder &Dec, const DecodedInsn &DI,
                         InstructionFlowScratch &Scratch) {
                       for (const InstructionRef &Ref :
                            Scratch.dataAccesses(Img, Dec, DI))
                         if (isDataAccess(Ref))
                           Slots[Index].emplace_back(Ref.To, Ref.Bytes);
                     })) {
    clear();
    return std::nullopt;
  }
  // A function counts once for each width it states at an address, and
  // only for a width that names data.
  std::vector<std::tuple<va_t, unsigned, int>> Changes;
  for (size_t Index = 0; Index < Read.size(); ++Index) {
    auto &Slot = Slots[Index];
    llvm::sort(Slot);
    Slot.erase(std::unique(Slot.begin(), Slot.end()), Slot.end());
    for (const auto &[Address, Width] : Slot)
      if (Width && Width <= MaxDataAccessWidth)
        Changes.emplace_back(Address, Width, Index < Dropped ? -1 : 1);
  }
  llvm::sort(Changes);
  std::vector<Count> Merged;
  Merged.reserve(Counts.size() + Changes.size());
  auto Kept = Counts.begin();
  for (size_t I = 0; I < Changes.size();) {
    const va_t Address = std::get<0>(Changes[I]);
    const unsigned Width = std::get<1>(Changes[I]);
    int64_t Total = 0;
    for (; I < Changes.size() && std::get<0>(Changes[I]) == Address &&
           std::get<1>(Changes[I]) == Width;
         ++I)
      Total += std::get<2>(Changes[I]);
    for (; Kept != Counts.end() &&
           std::pair(Kept->Address, Kept->Width) < std::pair(Address, Width);
         ++Kept)
      Merged.push_back(*Kept);
    if (Kept != Counts.end() && Kept->Address == Address &&
        Kept->Width == Width)
      Total += (Kept++)->Functions;
    assert(Total >= 0 && "an extent stops counting a width it never stated");
    if (Total > 0)
      Merged.push_back({Address, Width, static_cast<uint32_t>(Total)});
  }
  Merged.insert(Merged.end(), Kept, Counts.end());
  Counts = std::move(Merged);
  Extents = std::move(Next);
  Entries = std::move(NextEntries);
  DataAccessWidths Widths;
  for (const Count &Stated : Counts)
    Widths.add(Stated.Address, Stated.Width);
  return Widths;
}

const DataAccessWidths *Session::dataAccessWidths() {
  if (!Loaded || Img.Arch == Arch::EVM || Img.Arch == Arch::SBF ||
      !synchronizeFunctions())
    return nullptr;
  // The widths are read from the functions' extents: those they were read
  // from serve while each function keeps its entry and size.
  if (AccessWidths &&
      llvm::equal(Functions, AccessWidthsFunctions,
                  [](const FuncInfo &F, const std::pair<va_t, uint64_t> &E) {
                    return F.Entry == E.first && F.Size == E.second;
                  }))
    return &*AccessWidths;
  std::vector<std::pair<va_t, uint64_t>> Extents;
  Extents.reserve(Functions.size());
  for (const FuncInfo &F : Functions)
    Extents.emplace_back(F.Entry, F.Size);
  // C kept from before names data by the widths of other functions.
  if (AccessWidths)
    forgetEmittedSources();
  AccessWidths = AccessScan.update(Img, Functions);
  AccessWidthsFunctions = std::move(Extents);
  return AccessWidths ? &*AccessWidths : nullptr;
}

} // namespace neverd::sdk
