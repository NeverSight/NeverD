//===- InstructionFlow.cpp - What each native instruction does -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "InstructionFlow.h"

#include "SessionImpl.h"

#include "neverd/Limits.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/libc/NoReturnLoadedTarget.h"
#include "neverd/loader/ELF/MIPSCodeMode.h"
#include "neverd/loader/ExceptionEntryPoints.h"
#include "neverd/sdk/Limits.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <deque>
#include <iterator>
#include <limits>
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

/// Numeric facts about the original machine, kept separate from the pointer
/// certificates used by recompilation. A page fragment can determine the raw
/// address of a load without becoming an independently relocatable pointer.
class ReferenceValues {
public:
  enum class Role : uint8_t { Unknown, Scalar, Fragment, Address };
  struct Known {
    uint64_t Value;
    Role Kind;
    uint64_t Mask;
    bool operator==(const Known &) const = default;
  };

  std::optional<Known> known(const NdVar &V) const {
    if (!V.Size || V.Size > sizeof(uint64_t))
      return std::nullopt;
    if (V.isConst())
      return Known{V.Offset & (UINT64_MAX >> ((8 - V.Size) * 8)),
                   isExactAddressProvenance(V.Provenance) ? Role::Address
                   : V.Provenance == ConstantAddressProvenance::AddressFragment
                       ? Role::Fragment
                   : V.Provenance == ConstantAddressProvenance::Scalar
                       ? Role::Scalar
                       : Role::Unknown,
                   UINT64_MAX >> ((8 - V.Size) * 8)};
    for (const Slot &S : Values) {
      if (S.Variable.Space == V.Space && S.Variable.Offset == V.Offset &&
          S.Variable.Size == V.Size)
        return S.Fact;
      // Physical register lanes share their low bytes (EAX inside RAX, for
      // example). Temporaries have value identities, not register aliases.
      if (V.isReg() && S.Variable.isReg() && V.Offset >= S.Variable.Offset &&
          V.Offset - S.Variable.Offset < S.Variable.Size &&
          V.Size <= S.Variable.Size - (V.Offset - S.Variable.Offset))
        return Known{(S.Fact.Value >> ((V.Offset - S.Variable.Offset) * 8)) &
                         (UINT64_MAX >> ((8 - V.Size) * 8)),
                     Role::Scalar,
                     (S.Fact.Mask >> ((V.Offset - S.Variable.Offset) * 8)) &
                         (UINT64_MAX >> ((8 - V.Size) * 8))};
    }
    return std::nullopt;
  }

  std::optional<Known> value(const NdVar &V) const {
    const auto K = known(V);
    return K && K->Mask == (UINT64_MAX >> ((8 - V.Size) * 8)) ? K
                                                              : std::nullopt;
  }

  void clear() { Values.clear(); }
  void invalidate(const NdVar &Out) {
    llvm::erase_if(Values, [&](const Slot &S) {
      return S.Variable.Space == Out.Space &&
             S.Variable.Offset < Out.Offset + Out.Size &&
             Out.Offset < S.Variable.Offset + S.Variable.Size;
    });
  }
  void beginInstruction() {
    llvm::erase_if(Values, [](const Slot &S) { return S.Variable.isTemp(); });
  }
  void forgetUndefined(llvm::ArrayRef<LowUndefinedEffect> Effects,
                       size_t Completed) {
    for (const auto &Effect : Effects)
      if (Effect.AfterOp == Completed) {
        // An exact false guard preserves the old bits (for example an x86
        // zero-count shift). An unknown guard may still produce arbitrary
        // bits, so it cannot keep a constant result.
        if (Effect.When)
          if (auto Guard = value(*Effect.When); Guard && Guard->Value == 0)
            continue;
        invalidate(Effect.Output);
      }
  }
  void advance(llvm::ArrayRef<LowOp> Ops,
               llvm::ArrayRef<LowUndefinedEffect> Undefined) {
    beginInstruction();
    size_t Completed = 0;
    forgetUndefined(Undefined, Completed);
    for (const LowOp &Op : Ops) {
      advance(Op);
      forgetUndefined(Undefined, ++Completed);
    }
  }
  void canonicalizeRegisters() {
    beginInstruction();
    llvm::sort(Values, [](const Slot &A, const Slot &B) {
      return std::tie(A.Variable.Offset, A.Variable.Size) <
             std::tie(B.Variable.Offset, B.Variable.Size);
    });
  }
  bool intersect(const ReferenceValues &Other) {
    bool Changed = false;
    for (Slot &S : Values) {
      const Known Previous = S.Fact;
      const auto Incoming = Other.known(S.Variable);
      S.Fact.Mask = Incoming ? S.Fact.Mask & Incoming->Mask &
                                   ~(S.Fact.Value ^ Incoming->Value)
                             : 0;
      S.Fact.Value &= S.Fact.Mask;
      if (!Incoming || S.Fact.Kind != Incoming->Kind)
        S.Fact.Kind = Role::Unknown;
      Changed |= Previous != S.Fact;
    }
    llvm::erase_if(Values, [](const Slot &S) { return S.Fact.Mask == 0; });
    return Changed;
  }

  void advance(const LowOp &Op) {
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
        Op.Opcode == NdOp::INTRINSIC) {
      clear();
      return;
    }
    const NdVar &Out = Op.Output;
    if (!Out.Size || (!Out.isReg() && !Out.isTemp()))
      return;
    const bool Ordinary =
        Op.MemoryOrdering == NdMemoryOrdering::None &&
        Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
    const auto Bits =
        Ordinary ? lowScalarConstantResult(
                       Op,
                       [&](const NdVar &Input) -> std::optional<uint64_t> {
                         if (auto Fact = value(Input))
                           return Fact->Value;
                         return std::nullopt;
                       })
                 : std::nullopt;
    auto Result =
        Bits       ? std::optional<Known>{{*Bits, Role::Scalar,
                                           UINT64_MAX >> ((8 - Out.Size) * 8)}}
        : Ordinary ? partialResult(Op)
                   : std::nullopt;
    Role Kind = Role::Scalar;
    if (Result && Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
      if (auto Input = known(Op.Inputs[0]))
        Kind = Input->Kind;
    } else if (Bits && Op.NumInputs == 2 &&
               (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB)) {
      const auto Left = value(Op.Inputs[0]), Right = value(Op.Inputs[1]);
      if (Left && Right) {
        const auto IsBase = [](Role R) {
          return R == Role::Address || R == Role::Fragment;
        };
        if (IsBase(Left->Kind) && !IsBase(Right->Kind))
          Kind = Role::Address;
        else if (Op.Opcode == NdOp::INT_ADD && IsBase(Right->Kind) &&
                 !IsBase(Left->Kind))
          Kind = Role::Address;
        else if (Left->Kind == Role::Unknown && Right->Kind == Role::Unknown)
          Kind = Role::Unknown;
      }
    }
    // Arithmetic completes a known page/base fragment into a concrete
    // address for metadata xrefs (for example ADRP followed by ADD). This
    // numeric observation does not certify a relocatable C/LLVM pointer.
    invalidate(Out);
    if (Result) {
      Result->Kind = Kind;
      Values.push_back({Out, *Result});
    }
  }

private:
  // A byte written through a full-register mask/merge is still known when
  // the preserved bytes are not. Keep those facts without inventing the rest
  // of the register. Exact arithmetic remains owned by the scalar evaluator.
  std::optional<Known> partialResult(const LowOp &Op) const {
    const unsigned Width = Op.Output.Size * 8;
    if (Width > 64)
      return std::nullopt;
    const auto Bits = [&](const NdVar &V) {
      llvm::KnownBits K(V.Size * 8);
      if (auto F = known(V)) {
        K.Zero = llvm::APInt(V.Size * 8, F->Mask & ~F->Value);
        K.One = llvm::APInt(V.Size * 8, F->Mask & F->Value);
      }
      return K;
    };
    llvm::KnownBits Result(Width);
    if (Op.NumInputs == 1 && Op.Inputs[0].Size &&
        Op.Inputs[0].Size <= Op.Output.Size) {
      const auto Input = Bits(Op.Inputs[0]);
      if (Op.Opcode == NdOp::INT_ZEXT)
        Result = Input.zext(Width);
      else if (Op.Opcode == NdOp::INT_SEXT)
        Result = Input.sext(Width);
      else if (Input.getBitWidth() == Width) {
        if (Op.Opcode == NdOp::COPY)
          Result = Input;
        else if (Op.Opcode == NdOp::INT_NOT || Op.Opcode == NdOp::INT_NEGATE) {
          Result.Zero = Input.One;
          Result.One = Input.Zero;
        }
      }
    } else if (Op.NumInputs == 2 && Op.Inputs[0].Size == Op.Output.Size &&
               Op.Inputs[1].Size == Op.Output.Size) {
      const auto Left = Bits(Op.Inputs[0]), Right = Bits(Op.Inputs[1]);
      if (Op.Opcode == NdOp::INT_AND)
        Result = Left & Right;
      else if (Op.Opcode == NdOp::INT_OR)
        Result = Left | Right;
      else if (Op.Opcode == NdOp::INT_XOR)
        Result = Left ^ Right;
      else if (Right.isConstant() &&
               (Op.Opcode == NdOp::INT_LEFT || Op.Opcode == NdOp::INT_RIGHT ||
                Op.Opcode == NdOp::INT_ASHR)) {
        const unsigned Count = static_cast<unsigned>(
            std::min<uint64_t>(Right.getConstant().getZExtValue(), Width));
        Result = Left;
        // LowIR shifts saturate; ISA-specific count masking was lifted.
        if (Op.Opcode == NdOp::INT_LEFT) {
          Result <<= Count;
          Result.Zero.setLowBits(Count);
        } else if (Op.Opcode == NdOp::INT_RIGHT) {
          Result >>= Count;
          Result.Zero.setHighBits(Count);
        } else {
          Result.Zero = Left.Zero.ashr(std::min(Count, Width - 1));
          Result.One = Left.One.ashr(std::min(Count, Width - 1));
        }
      }
    }
    const uint64_t Mask = (Result.Zero | Result.One).getZExtValue();
    if (!Mask)
      return std::nullopt;
    return Known{Result.One.getZExtValue(), Role::Scalar, Mask};
  }

  struct Slot {
    NdVar Variable;
    Known Fact;
  };
  llvm::SmallVector<Slot, 8> Values;
};

InstructionFlow
summarizeLowFlow(const BinaryImage &Img, llvm::ArrayRef<LowOp> Ops, va_t Next,
                 va_t CodeReference, ReferenceValues &Values,
                 const libc::NoReturnTargetIndex *NoReturnTargets = nullptr,
                 llvm::ArrayRef<LowUndefinedEffect> Undefined = {});

} // namespace

llvm::SmallVector<InstructionRef, 2>
InstructionFlowScratch::dataAccesses(const BinaryImage &Img, Decoder &Dec,
                                     const DecodedInsn &DI) {
  if ((Img.Arch == Arch::X86 || Img.Arch == Arch::X64) && Dec.detailEnabled() &&
      Dec.textEnabled() && DI.Raw && DI.Raw->detail) {
    // LEA computes an address without accessing memory.
    if (DI.Id == X86_INS_LEA) {
      Ops.clear();
      return {};
    }
    // These scalar families access memory only through an explicit operand.
    // A plain [SP/BP + displacement] has an unknown base when lifted as a
    // single instruction. It cannot supply an image address to this scan.
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
                       if (Op.type != X86_OP_MEM)
                         return Op.type == X86_OP_REG || Op.type == X86_OP_IMM;
                       if (Op.mem.segment != X86_REG_INVALID ||
                           Op.mem.index != X86_REG_INVALID)
                         return false;
                       switch (Op.mem.base) {
                       case X86_REG_SP:
                       case X86_REG_ESP:
                       case X86_REG_RSP:
                       case X86_REG_BP:
                       case X86_REG_EBP:
                       case X86_REG_RBP:
                         return true;
                       default:
                         return false;
                       }
                     })) {
      Ops.clear();
      return {};
    }
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
                                  const TargetRegInfo *StackRegisters,
                                  LowInstructionUndefinedEffects *Undefined) {
  InstructionFlow Flow;
  Ops.clear();
  // Each row is lifted independently of its neighbours.
  Dec.resetX86FpuState();
  try {
    // The relocations inside the instruction make its relocated immediates
    // addresses, as they are to the pipeline.
    const InstructionRelocations Relocations = instructionRelocations(Img, DI);
    if (isMIPS(Img.Arch)) {
      if (!UnitDecoder || UnitArch != Img.Arch) {
        UnitDecoder = std::make_unique<Decoder>();
        if (!UnitDecoder->init(Img)) {
          UnitDecoder.reset();
          Flow.Kind = UnliftedFlow;
          Flow.Stack.Unknown = true;
          return Flow;
        }
        UnitArch = Img.Arch;
      }
      const auto *Segment = Img.getSegmentFor(DI.Addr);
      const size_t Offset = DI.Addr - Segment->VA;
      DecodedExecutionUnit Unit;
      if (!UnitDecoder->selectMode(Img, DI.Addr, Dec.currentMode()) ||
          !UnitDecoder->decodeUnitForLift(Segment->Data.data() + Offset,
                                          Segment->Data.size() - Offset,
                                          DI.Addr, Unit) ||
          !mips::codeModeRangeMatches(Img, DI.Addr, Unit.spanBytes(),
                                      UnitDecoder->currentMode())) {
        Flow.Kind = UnliftedFlow;
        Flow.Stack.Unknown = true;
        return Flow;
      }
      const auto Slot = UnitDecoder->liftUnitToLow(Unit, Ops);
      if (Slot) {
        Flow.DelaySlotBytes = Slot->Size;
        // The listing reports the slot's memory and stack effects in its
        // own physical row. The head still uses the validated unit's transfer.
        llvm::erase_if(Ops,
                       [&](const LowOp &Op) { return Op.Addr != DI.Addr; });
      }
    } else {
      Dec.liftToLow(DI, Ops, Relocations.Addresses, Relocations.Scalars,
                    Undefined);
    }
  } catch (const UnliftedInstruction &) {
    // Nothing is known about what the instruction does, so it states no
    // transfer, no reference and no stack pointer.
    Flow.Kind = UnliftedFlow;
    Flow.Stack.Unknown = true;
    Ops.clear();
    return Flow;
  }
  ReferenceValues Values;
  const auto DelaySlotBytes = Flow.DelaySlotBytes;
  Flow = summarizeLowFlow(Img, Ops, DI.Addr + DI.Size + DelaySlotBytes,
                          Dec.pcRelCodeRefTarget(DI), Values, NoReturnTargets,
                          Undefined ? llvm::ArrayRef(Undefined->Effects)
                                    : llvm::ArrayRef<LowUndefinedEffect>());
  Flow.DelaySlotBytes = DelaySlotBytes;
  if (DelaySlotBytes && Flow.Kind == "jump" &&
      llvm::any_of(Ops,
                   [](const LowOp &Op) { return Op.Opcode == NdOp::COND_BR; }))
    Flow.Kind = "cjump";
  if (StackRegisters)
    Flow.Stack = stackMove(*StackRegisters, Ops);
  return Flow;
}

namespace {
InstructionFlow
summarizeLowFlow(const BinaryImage &Img, llvm::ArrayRef<LowOp> Ops, va_t Next,
                 va_t CodeReference, ReferenceValues &Values,
                 const libc::NoReturnTargetIndex *NoReturnTargets,
                 llvm::ArrayRef<LowUndefinedEffect> Undefined) {
  InstructionFlow Flow;
  Values.beginInstruction();
  llvm::SmallVector<va_t, 2> Offsets;
  const auto ConstantInput = [](const LowOp &Op) -> std::optional<va_t> {
    if (Op.NumInputs == 0 || !Op.Inputs[0].isConst())
      return std::nullopt;
    return static_cast<va_t>(Op.Inputs[0].Offset);
  };
  size_t Completed = 0;
  Values.forgetUndefined(Undefined, Completed);
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
      if (!Memory.Complete || !Memory.Address ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        break;
      if (const auto Address = Values.value(*Memory.Address); Address)
        Flow.Refs.push_back({Address->Value,
                             Op.Opcode == NdOp::LOAD ? "read" : "write",
                             Memory.AccessSize});
      // A stored address, such as an argument pushed as `offset aText`.
      if (Op.Opcode == NdOp::STORE && Memory.StoredValue)
        if (const auto Stored = Values.value(*Memory.StoredValue);
            Stored && Stored->Kind == ReferenceValues::Role::Address)
          Offsets.push_back(Stored->Value);
      break;
    }
    default:
      break;
    }
    Values.advance(Op);
    Values.forgetUndefined(Undefined, ++Completed);
    // An address the instruction puts in a register, as `mov eax, offset X`.
    if (Op.Output.isReg())
      if (const auto Value = Values.value(Op.Output);
          Value && Value->Kind == ReferenceValues::Role::Address)
        Offsets.push_back(Value->Value);
  }
  // A predicated call's unexecuted arm still reaches the next instruction.
  // The lift carries that guard even when its last transfer is the call.
  if (Flow.NoReturn && llvm::any_of(Ops, [](const LowOp &Op) {
        return Op.Opcode == NdOp::COND_BR;
      }))
    Flow.NoReturn = false;
  if (va_t Address = CodeReference; Address != InvalidVA)
    Offsets.push_back(Address);
  // The next instruction's address, which a call pushes and a system call
  // saves, is where execution returns, not an address the code refers to.
  for (const va_t Address : Offsets)
    if (Address != Next && Address != Flow.Target &&
        llvm::none_of(Flow.Refs, [&](const InstructionRef &Ref) {
          return Ref.To == Address && Ref.Kind == "offset";
        }))
      Flow.Refs.push_back({Address, "offset"});
  return Flow;
}
} // namespace

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

std::shared_ptr<const std::vector<va_t>>
functionEntries(llvm::ArrayRef<const FuncInfo *> Ordered) {
  auto Entries = std::make_shared<std::vector<va_t>>();
  Entries->reserve(Ordered.size());
  for (const FuncInfo *F : Ordered)
    Entries->push_back(F->Entry);
  return Entries;
}

FunctionPage pageFunctions(llvm::ArrayRef<const FuncInfo *> Ordered,
                           std::shared_ptr<const std::vector<va_t>> Entries,
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
  Page.Entries = std::move(Entries);
  return Page;
}
} // namespace

FunctionPage functionPage(const Session &S, va_t FirstEntry, int MaxFunctions) {
  if (!S.FunctionPages || S.FunctionPages->Revision != S.FunctionListRevision ||
      S.FunctionPages->List != S.Functions.data() ||
      S.FunctionPages->Count != S.Functions.size()) {
    auto Ordered = orderedFunctions(S.Functions);
    auto Entries = functionEntries(Ordered);
    S.FunctionPages = Session::FunctionPageIndex{
        S.FunctionListRevision, S.Functions.data(), S.Functions.size(),
        std::move(Ordered), std::move(Entries)};
  }
  return pageFunctions(S.FunctionPages->Ordered, S.FunctionPages->Entries,
                       FirstEntry, MaxFunctions);
}

FunctionPage functionPage(llvm::ArrayRef<FuncInfo> Functions, va_t FirstEntry,
                          int MaxFunctions) {
  const auto Ordered = orderedFunctions(Functions);
  return pageFunctions(Ordered, functionEntries(Ordered), FirstEntry,
                       MaxFunctions);
}

namespace {

/// The instructions of a function without a recorded size: those its own
/// direct jumps reach from \p Entry, short of another function's entry in
/// \p Entries, in address order.  32-bit ARM, whose jumps can change the
/// instruction set, reads up to the first terminator instead.
std::vector<va_t> unsizedBody(
    const BinaryImage &Img, Decoder &Dec, InstructionFlowScratch &Scratch,
    va_t Entry, llvm::ArrayRef<va_t> Entries, const std::function<bool()> &Stop,
    FunctionDecodeStatus &Status,
    llvm::function_ref<void(llvm::ArrayRef<InstructionRef>)> VisitRefs = {}) {
  const auto OtherEntry = [&](va_t Address) {
    return Address != Entry &&
           std::binary_search(Entries.begin(), Entries.end(), Address);
  };
  const bool FollowsJumps = Img.Arch != Arch::ARM && !Entries.empty();
  std::set<va_t> Body;
  std::vector<va_t> Pending{Entry};
  while (!Pending.empty() && !(Stop && Stop())) {
    const va_t Start = Pending.back();
    Pending.pop_back();
    bool Terminated = false;
    const bool ModeKnown = decodeNativeRange(
        Img, Dec, Start, 0, std::numeric_limits<uint64_t>::max(),
        [&](const DecodedInsn &DI, const uint8_t *, int) {
          if ((Stop && Stop()) || OtherEntry(DI.Addr) ||
              !Body.insert(DI.Addr).second) {
            Terminated = true;
            return false;
          }
          Terminated = Dec.isFunctionTerminator(DI);
          if (FollowsJumps || isMIPS(Img.Arch)) {
            const InstructionFlow Flow = Scratch.summarize(Img, Dec, DI);
            if (VisitRefs) {
              VisitRefs(Flow.Refs);
              ++Status.Instructions;
              Status.Bytes += DI.Size;
            }
            Status.Incomplete |=
                Flow.Kind == UnliftedFlow || Flow.Kind == "ijump";
            if (FollowsJumps && (Flow.Kind == "jump" || Flow.Kind == "cjump") &&
                Flow.Target != InvalidVA && !OtherEntry(Flow.Target) &&
                !Body.count(Flow.Target))
              Pending.push_back(Flow.Target);
            // A straight-line terminator can end only the current block:
            // AArch64 B.cond shares B's decode classification. The ordinary
            // lift's conditional flow still reaches the next instruction.
            if (Flow.Kind == "cjump")
              Terminated = false;
            Terminated |= Flow.NoReturn;
            if (Terminated && Flow.DelaySlotBytes)
              Body.insert(DI.Addr + DI.Size);
          }
          return !Terminated;
        });
    Status.Incomplete |= !ModeKnown || !Terminated;
  }
  Status.Incomplete |= !Pending.empty();
  Status.Stopped = Stop && Stop();
  if (VisitRefs)
    return {};
  return {Body.begin(), Body.end()};
}

/// decodeFunctions over the functions of \p Extents, each an entry and its
/// recorded size; \p Entries are every function's entries.
using OutlyingChunks = std::map<va_t, std::vector<ExceptionAddressRange>>;

bool decodeExtents(
    const BinaryImage &Img, llvm::ArrayRef<std::pair<va_t, uint64_t>> Extents,
    llvm::ArrayRef<va_t> Entries,
    llvm::function_ref<void(size_t, llvm::ArrayRef<InstructionRef>)> VisitRefs,
    llvm::function_ref<void(size_t Index, Decoder &, const DecodedInsn &,
                            InstructionFlowScratch &)>
        Visit,
    const std::function<bool()> &Stop = {},
    std::vector<FunctionDecodeStatus> *Status = nullptr,
    const libc::NoReturnTargetIndex *NoReturnTargets = nullptr,
    llvm::function_ref<void(size_t, const FunctionDecodeStatus &)> Finish = {},
    const OutlyingChunks *Chunks = nullptr) {
  std::vector<FunctionDecodeStatus> Reports(Extents.size());
  // The parts of a function its unwind records keep apart from its body,
  // past other functions' code, are its code too: MSVC moves cold paths
  // there.
  OutlyingChunks LocalChunks;
  if (!Chunks) {
    for (const auto &Chunk : Img.ExceptionMetadata.outlyingChunks())
      LocalChunks[Chunk.Function].push_back(Chunk.Range);
    Chunks = &LocalChunks;
  }
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
      } else if (VisitRefs && !Entries.empty() &&
                 (Img.Arch == Arch::X86 || Img.Arch == Arch::X64)) {
        // Each visited x86 row already has an independent LowIR lift. Widths
        // are order-independent and x86 has neither delay slots nor IT state.
        unsizedBody(Img, Local, Scratch, Entry, Entries, Stop, Report,
                    [&](auto Refs) { VisitRefs(Index, Refs); });
      } else {
        for (const va_t Address :
             unsizedBody(Img, Local, Scratch, Entry, Entries, Stop, Report)) {
          if (Stop && Stop())
            break;
          ReadRange(Address, 0, 1);
        }
      }
      if (const auto It = Chunks->find(Entry); It != Chunks->end())
        for (const ExceptionAddressRange &Range : It->second) {
          if (Stop && Stop())
            break;
          ReadRange(Range.Begin, Range.End - Range.Begin,
                    Range.End - Range.Begin);
        }
      if (Finish)
        Finish(Index, Report);
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
  return decodeExtents(Img, Extents, *Page.Entries, {}, Visit, Stop, Status,
                       NoReturnTargets);
}

struct ReferenceImageIndex {
  libc::NoReturnTargetIndex NoReturnTargets;
  std::vector<va_t> Independent;
  std::vector<ExceptionAddressRange> Exceptional;
  OutlyingChunks Chunks;

  explicit ReferenceImageIndex(const BinaryImage &Img) : NoReturnTargets(Img) {
    // Address-taken code and other function entries can arrive with independent
    // register state. Build this image inventory once, not once per function.
    for (va_t Slot : Img.CodePtrRelocSlots)
      if (auto Target = relocatedPointer(Img, Slot))
        Independent.push_back(*Target);
    // Handler and continuation entries start with unknown registers. Incomplete
    // tables cannot establish where those entries are; retain instruction-local
    // references in their ranges instead of assuming ordinary edges suffice.
    for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions) {
      visitExceptionEntryPoints(F, [&](va_t Address, ExceptionEntryKind) {
        Independent.push_back(Address);
      });
      if (!hasCompleteExceptionEntryInventory(F)) {
        Exceptional.push_back(F.CodeRange);
        Exceptional.insert(Exceptional.end(), F.FragmentRanges.begin(),
                           F.FragmentRanges.end());
      }
    }
    llvm::sort(Independent);
    Independent.erase(std::unique(Independent.begin(), Independent.end()),
                      Independent.end());
    llvm::sort(Exceptional,
               [](const auto &A, const auto &B) { return A.Begin < B.Begin; });
    size_t Ranges = 0;
    for (const auto &Range : Exceptional) {
      if (Ranges && Range.Begin <= Exceptional[Ranges - 1].End)
        Exceptional[Ranges - 1].End =
            std::max(Exceptional[Ranges - 1].End, Range.End);
      else
        Exceptional[Ranges++] = Range;
    }
    Exceptional.resize(Ranges);
    for (const auto &Chunk : Img.ExceptionMetadata.outlyingChunks())
      Chunks[Chunk.Function].push_back(Chunk.Range);
  }
};

namespace {
bool decodeFunctionReferences(
    const BinaryImage &Img, const FunctionPage &Page,
    const ReferenceImageIndex &ImageIndex,
    llvm::function_ref<void(size_t, va_t, llvm::StringRef,
                            const InstructionFlow &, unsigned, InstructionMode)>
        Visit,
    std::vector<FunctionDecodeStatus> *Coverage = nullptr) {
  struct Instruction {
    va_t Address, Next, CodeReference;
    InstructionMode Mode;
    std::string Text;
    InstructionFlow Direct;
    size_t OpsBegin, OpsCount;
    std::vector<LowUndefinedEffect> Undefined;
    bool Fallthrough, Transfer;
  };
  struct Sweep {
    std::vector<Instruction> Instructions;
    std::vector<LowOp> Ops;
    bool DirectOnly = false;
  };
  std::vector<Sweep> Sweeps(Page.Functions.size());
  std::vector<std::pair<va_t, uint64_t>> Extents;
  Extents.reserve(Page.Functions.size());
  for (const FuncInfo *F : Page.Functions)
    Extents.emplace_back(F->Entry, F->Size);
  const auto &NoReturnTargets = ImageIndex.NoReturnTargets;
  const auto &Independent = ImageIndex.Independent;
  const auto &Exceptional = ImageIndex.Exceptional;
  const auto HasExceptionalFlow = [&](va_t Address) {
    auto It = std::upper_bound(
        Exceptional.begin(), Exceptional.end(), Address,
        [](va_t A, const ExceptionAddressRange &R) { return A < R.Begin; });
    return It != Exceptional.begin() && std::prev(It)->contains(Address);
  };
  const auto PublishDirect = [&](size_t Index) {
    auto &S = Sweeps[Index];
    for (const auto &I : S.Instructions)
      Visit(Index, I.Address, I.Text, I.Direct, I.Next - I.Address, I.Mode);
    // Retire per-function storage inside its worker, also when an unresolved
    // edge makes the remaining traversal instruction-local.
    const bool DirectOnly = S.DirectOnly;
    S = {};
    S.DirectOnly = DirectOnly;
  };
  return decodeExtents(
      Img, Extents, *Page.Entries, {},
      [&](size_t Index, Decoder &Dec, const DecodedInsn &DI,
          InstructionFlowScratch &Scratch) {
        auto &S = Sweeps[Index];
        LowInstructionUndefinedEffects Undefined;
        const bool X86 = Img.Arch == Arch::X86 || Img.Arch == Arch::X64;
        InstructionFlow Flow = Scratch.summarize(
            Img, Dec, DI, nullptr, X86 && !S.DirectOnly ? &Undefined : nullptr);
        std::string Text = DI.Raw ? DI.Raw->mnemonic : "";
        if (DI.Raw && *DI.Raw->op_str)
          Text += std::string(" ") + DI.Raw->op_str;
        const auto Ops = Scratch.operations();
        // ARM IT state and mode-changing flow are not ordinary native edges.
        // An unlifted or unresolved transfer can enter any part of a body;
        // carrying values only along its known neighbours would be unsound.
        bool Unresolved = Img.Arch == Arch::ARM || Flow.Kind == UnliftedFlow ||
                          Flow.Kind == "ijump" || HasExceptionalFlow(DI.Addr);
        for (size_t O = 0; O < Ops.size(); ++O)
          if (Ops[O].Opcode == NdOp::BRANCH || Ops[O].Opcode == NdOp::COND_BR)
            Unresolved |= O + 1 != Ops.size() || Flow.Target == InvalidVA;
        if (Unresolved && !S.DirectOnly) {
          S.DirectOnly = true;
          PublishDirect(Index);
        }
        if (S.DirectOnly) {
          Visit(Index, DI.Addr, Text, Flow, DI.Size, Dec.currentMode());
          return;
        }
        const bool Fallthrough =
            !Flow.NoReturn && Flow.Kind != "jump" && Flow.Kind != "ret" &&
            (Flow.Kind == "cjump" || Flow.Kind == "call" ||
             Flow.Kind == "icall" || !Dec.isFunctionTerminator(DI) ||
             Dec.isResumableTrap(DI));
        S.Instructions.push_back(
            {DI.Addr, DI.Addr + DI.Size, Dec.pcRelCodeRefTarget(DI),
             Dec.currentMode(), std::move(Text), std::move(Flow), S.Ops.size(),
             Ops.size(), std::move(Undefined.Effects), Fallthrough,
             !X86 || Undefined.Coverage == LowUndefinedCoverage::Complete});
        S.Ops.insert(S.Ops.end(), Ops.begin(), Ops.end());
      },
      {}, Coverage, &NoReturnTargets,
      [&](size_t Index, const FunctionDecodeStatus &Status) {
        auto &S = Sweeps[Index];
        if (S.DirectOnly || S.Instructions.empty())
          return;
        if (Status.Incomplete || Status.Stopped) {
          PublishDirect(Index);
          return;
        }
        const size_t Count = S.Instructions.size();
        llvm::DenseMap<va_t, size_t> At;
        At.reserve(Count);
        for (size_t I = 0; I < Count; ++I)
          if (!At.try_emplace(S.Instructions[I].Address, I).second) {
            PublishDirect(Index);
            return;
          }
        // An independently callable entry in the middle of a decoded
        // instruction needs an overlapping decode, not inherited values from
        // the surrounding function. This matters for sized x86 bodies.
        const auto [Entry, Size] = Extents[Index];
        for (const auto *Roots : {Page.Entries.get(), &Independent})
          for (auto It = std::lower_bound(Roots->begin(), Roots->end(), Entry);
               It != Roots->end() && *It - Entry < Size; ++It)
            if (!At.count(*It)) {
              PublishDirect(Index);
              return;
            }
        std::vector<bool> Leader(Count, false), Root(Count, false);
        Leader[0] = true;
        const auto IndependentEntry = [&](va_t Address) {
          if (auto It = At.find(Address); It != At.end())
            Leader[It->second] = Root[It->second] = true;
        };
        IndependentEntry(Extents[Index].first);
        for (size_t I = 0; I < Count; ++I) {
          const auto &N = S.Instructions[I];
          if (std::binary_search(Independent.begin(), Independent.end(),
                                 N.Address) ||
              std::binary_search(Page.Entries->begin(), Page.Entries->end(),
                                 N.Address))
            Leader[I] = Root[I] = true;
          if (I + 1 < Count) {
            if (!N.Direct.Kind.empty() || !N.Fallthrough)
              Leader[I + 1] = true;
            if (N.Next != S.Instructions[I + 1].Address)
              Leader[I + 1] = true;
          }
          if (N.Direct.Kind == "jump" || N.Direct.Kind == "cjump") {
            if (auto It = At.find(N.Direct.Target); It != At.end()) {
              Leader[It->second] = true;
            } else {
              // A target inside the sized body but not at one of its decoded
              // instruction boundaries needs another decode/control-flow
              // model. It is not an ordinary tail exit from this graph.
              const auto [Entry, Size] = Extents[Index];
              if (N.Direct.Target >= Entry && N.Direct.Target - Entry < Size) {
                PublishDirect(Index);
                return;
              }
            }
          } else if (N.Direct.Kind == "call") {
            IndependentEntry(N.Direct.Target);
          }
          for (const InstructionRef &Ref : N.Direct.Refs)
            if (Ref.Kind == "offset")
              IndependentEntry(Ref.To);
          if (N.Fallthrough)
            if (auto It = At.find(N.Next); It != At.end())
              if (It->second != I + 1)
                Leader[It->second] = true;
        }
        struct Block {
          size_t Begin, End;
          llvm::SmallVector<size_t, 2> Successors;
          ReferenceValues Incoming;
          size_t Predecessors = 0;
          bool Seen = false, Queued = false;
        };
        std::vector<Block> Blocks;
        std::vector<size_t> BlockOf(Count);
        for (size_t I = 0; I < Count; ++I) {
          if (Leader[I])
            Blocks.push_back({I, I + 1, {}, {}, 0, false, false});
          BlockOf[I] = Blocks.size() - 1;
          Blocks.back().End = I + 1;
        }
        for (auto &B : Blocks) {
          const auto &N = S.Instructions[B.End - 1];
          const auto Edge = [&](va_t Address) {
            if (auto It = At.find(Address); It != At.end()) {
              const auto To = BlockOf[It->second];
              if (!llvm::is_contained(B.Successors, To)) {
                B.Successors.push_back(To);
                ++Blocks[To].Predecessors;
              }
            }
          };
          if (N.Direct.Kind == "jump" || N.Direct.Kind == "cjump")
            Edge(N.Direct.Target);
          if (N.Fallthrough)
            Edge(N.Next);
        }
        std::deque<size_t> Pending;
        const auto Queue = [&](size_t B) {
          if (!Blocks[B].Queued) {
            Blocks[B].Queued = true;
            Pending.push_back(B);
          }
        };
        for (size_t B = 0; B < Blocks.size(); ++B)
          if (Root[Blocks[B].Begin] || !Blocks[B].Predecessors) {
            Blocks[B].Seen = true;
            Queue(B);
          }
        const auto Operations = [&](const Instruction &N) {
          return llvm::ArrayRef(S.Ops).slice(N.OpsBegin, N.OpsCount);
        };
        // Unseen is lattice bottom; a seen empty map means all registers are
        // unknown. A join can only lose facts. Backedges therefore converge
        // without recursion, a distance cutoff or a state/work budget.
        while (!Pending.empty()) {
          const size_t B = Pending.front();
          Pending.pop_front();
          auto &Current = Blocks[B];
          Current.Queued = false;
          auto Values = Current.Incoming;
          for (size_t I = Current.Begin; I < Current.End; ++I) {
            const auto &N = S.Instructions[I];
            if (N.Transfer)
              Values.advance(Operations(N), N.Undefined);
            else
              Values.clear();
          }
          Values.canonicalizeRegisters();
          for (size_t To : Current.Successors) {
            auto &Next = Blocks[To];
            if (!Next.Seen) {
              Next.Seen = true;
              Next.Incoming = Values;
              Queue(To);
            } else if (Next.Incoming.intersect(Values)) {
              Queue(To);
            }
          }
        }
        // Publish once from the settled inputs. An optimistic first visit to
        // a join or loop is not a reference that may escape this analysis.
        for (const auto &B : Blocks) {
          auto Values = B.Incoming;
          for (size_t I = B.Begin; I < B.End; ++I) {
            const auto &N = S.Instructions[I];
            if (B.Seen && N.Transfer) {
              const auto Flow =
                  summarizeLowFlow(Img, Operations(N), N.Next, N.CodeReference,
                                   Values, &NoReturnTargets, N.Undefined);
              Visit(Index, N.Address, N.Text, Flow, N.Next - N.Address, N.Mode);
            } else {
              Values.clear();
              Visit(Index, N.Address, N.Text, N.Direct, N.Next - N.Address,
                    N.Mode);
            }
          }
        }
        S = {};
      },
      &ImageIndex.Chunks);
}

} // namespace

bool decodeFunctionReferences(
    const BinaryImage &Img, const FunctionPage &Page,
    llvm::function_ref<void(size_t, va_t, llvm::StringRef,
                            const InstructionFlow &, unsigned, InstructionMode)>
        Visit) {
  const ReferenceImageIndex Index(Img);
  return decodeFunctionReferences(Img, Page, Index, Visit);
}

const ReferenceImageIndex &Session::referenceImageIndex() const {
  if (!ReferenceImage)
    ReferenceImage = std::make_shared<ReferenceImageIndex>(Img);
  return *ReferenceImage;
}

bool decodeFunctionReferences(
    const Session &S, const FunctionPage &Page,
    llvm::function_ref<void(size_t, va_t, llvm::StringRef,
                            const InstructionFlow &, unsigned, InstructionMode)>
        Visit,
    std::vector<FunctionDecodeStatus> *Status) {
  return decodeFunctionReferences(S.Img, Page, S.referenceImageIndex(), Visit,
                                  Status);
}

std::optional<DataAccessWidths>
collectDataAccessWidths(const BinaryImage &Img,
                        llvm::ArrayRef<FuncInfo> Functions) {
  std::vector<std::pair<va_t, unsigned>> Accesses;
  const auto Ordered = orderedFunctions(Functions);
  const auto Entries = functionEntries(Ordered);
  std::optional<libc::NoReturnTargetIndex> NoReturnTargets;
  if (llvm::any_of(Ordered, [](const FuncInfo *F) { return !F->Size; }))
    NoReturnTargets.emplace(Img);
  for (std::optional<va_t> Cursor = va_t{0}; Cursor;) {
    const FunctionPage Page = pageFunctions(Ordered, Entries, *Cursor,
                                            FunctionPage::MaxFunctionsPerPage);
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
  const bool EntriesChanged = NextEntries != Entries;
  // Sized bodies depend only on their extents. Unsized bodies also stop at
  // other entries, including those reached by a jump: a changed entry list
  // replaces their contributions together, without decoding the old bodies
  // against either the old or the new boundaries.
  std::vector<std::pair<va_t, uint64_t>> Read, Added;
  auto Before = Extents.begin(), After = Next.begin();
  while (Before != Extents.end() || After != Next.end()) {
    if (After == Next.end() || (Before != Extents.end() && *Before < *After)) {
      if (!EntriesChanged || Before->second)
        Read.push_back(*Before);
      ++Before;
    } else if (Before == Extents.end() || *After < *Before) {
      Added.push_back(*After++);
    } else {
      if (EntriesChanged && !After->second)
        Added.push_back(*After);
      ++Before;
      ++After;
    }
  }
  const size_t Dropped = Read.size();
  if (Read.empty())
    Read = std::move(Added);
  else
    Read.insert(Read.end(), Added.begin(), Added.end());
  // Each extent's accesses, which only the thread decoding it writes.
  std::vector<std::vector<std::pair<va_t, unsigned>>> Slots(Read.size());
  const auto Collect = [&](size_t Index, llvm::ArrayRef<InstructionRef> Refs) {
    for (const InstructionRef &Ref : Refs)
      if (isDataAccess(Ref))
        Slots[Index].emplace_back(Ref.To, Ref.Bytes);
  };
  if (!Read.empty() &&
      !decodeExtents(Img, Read, NextEntries, Collect,
                     [&](size_t Index, Decoder &Dec, const DecodedInsn &DI,
                         InstructionFlowScratch &Scratch) {
                       Collect(Index, Scratch.dataAccesses(Img, Dec, DI));
                     })) {
    clear();
    return std::nullopt;
  }
  // A function counts once for each width it states at an address, and
  // only for a width that names data.
  std::vector<std::tuple<va_t, unsigned, int, bool>> Changes;
  for (size_t Index = 0; Index < Read.size(); ++Index) {
    auto &Slot = Slots[Index];
    llvm::sort(Slot);
    Slot.erase(std::unique(Slot.begin(), Slot.end()), Slot.end());
    for (const auto &[Address, Width] : Slot)
      if (Width && Width <= MaxDataAccessWidth)
        Changes.emplace_back(Address, Width, Index < Dropped ? -1 : 1,
                             Read[Index].second == 0);
  }
  llvm::sort(Changes);
  if (EntriesChanged)
    llvm::erase_if(Counts, [](Count &Stated) {
      Stated.Functions -= Stated.UnsizedFunctions;
      Stated.UnsizedFunctions = 0;
      return Stated.Functions == 0;
    });
  std::vector<Count> Merged;
  Merged.reserve(Counts.size() + Changes.size());
  auto Kept = Counts.begin();
  for (size_t I = 0; I < Changes.size();) {
    const va_t Address = std::get<0>(Changes[I]);
    const unsigned Width = std::get<1>(Changes[I]);
    int64_t Total = 0, Unsized = 0;
    for (; I < Changes.size() && std::get<0>(Changes[I]) == Address &&
           std::get<1>(Changes[I]) == Width;
         ++I) {
      Total += std::get<2>(Changes[I]);
      if (std::get<3>(Changes[I]))
        Unsized += std::get<2>(Changes[I]);
    }
    for (; Kept != Counts.end() &&
           std::pair(Kept->Address, Kept->Width) < std::pair(Address, Width);
         ++Kept)
      Merged.push_back(*Kept);
    if (Kept != Counts.end() && Kept->Address == Address &&
        Kept->Width == Width) {
      Total += Kept->Functions;
      Unsized += (Kept++)->UnsizedFunctions;
    }
    assert(Total >= 0 && Unsized >= 0 && Unsized <= Total &&
           "an extent stops counting a width it never stated");
    if (Total > 0)
      Merged.push_back({Address, Width, static_cast<uint32_t>(Total),
                        static_cast<uint32_t>(Unsized)});
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
