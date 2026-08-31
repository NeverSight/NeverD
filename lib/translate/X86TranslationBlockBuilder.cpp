//===- X86TranslationBlockBuilder.cpp - Exact x86 block construction -----===//

#include "neverd/translate/X86TranslationBlockBuilder.h"

#include "neverd/decode/Decoder.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::translate {

char X86TranslationBlockBuilderError::ID;

X86TranslationBlockBuilderError::X86TranslationBlockBuilderError(
    X86TranslationBlockBuilderErrorCode Code, uint64_t GuestPC,
    std::optional<GuestMemoryFault> Fault, std::string Detail)
    : Code(Code), GuestPC(GuestPC), Fault(Fault), Detail(std::move(Detail)) {}

void X86TranslationBlockBuilderError::log(llvm::raw_ostream &OS) const {
  switch (Code) {
  case X86TranslationBlockBuilderErrorCode::DecoderInitializationFailed:
    OS << "x86-64 translation decoder initialization failed";
    break;
  case X86TranslationBlockBuilderErrorCode::InstructionFetchFailed:
    OS << "x86-64 translation instruction fetch failed";
    break;
  case X86TranslationBlockBuilderErrorCode::TruncatedInstruction:
    OS << "x86-64 translation instruction is truncated";
    break;
  case X86TranslationBlockBuilderErrorCode::UndecodableInstruction:
    OS << "x86-64 translation instruction is undecodable";
    break;
  case X86TranslationBlockBuilderErrorCode::UnliftedInstruction:
    OS << "x86-64 translation instruction has no strict LowIR lift";
    break;
  case X86TranslationBlockBuilderErrorCode::GuestAddressOverflow:
    OS << "x86-64 translation instruction range overflows";
    break;
  case X86TranslationBlockBuilderErrorCode::InconsistentDecode:
    OS << "x86-64 translation decoder returned an inconsistent length";
    break;
  case X86TranslationBlockBuilderErrorCode::InconsistentControl:
    OS << "x86-64 translation control metadata is inconsistent";
    break;
  case X86TranslationBlockBuilderErrorCode::ExecutableBytesChanged:
    OS << "x86-64 translation bytes changed while constructing the block";
    break;
  case X86TranslationBlockBuilderErrorCode::InvalidDescriptor:
    OS << "x86-64 translation block descriptor is invalid";
    break;
  case X86TranslationBlockBuilderErrorCode::InstructionBudgetExceeded:
    OS << "x86-64 translation instruction budget is exhausted";
    break;
  }
  OS << " at guest PC 0x" << llvm::utohexstr(GuestPC);
  if (Fault)
    OS << " (runtime memory fault " << static_cast<uint32_t>(Fault->Kind)
       << ')';
  if (!Detail.empty())
    OS << ": " << Detail;
}

std::error_code X86TranslationBlockBuilderError::convertToErrorCode() const {
  return std::make_error_code(std::errc::invalid_argument);
}

namespace {

constexpr uint64_t kMaximumX86InstructionSize = 15;

llvm::Error failure(X86TranslationBlockBuilderErrorCode Code, uint64_t GuestPC,
                    std::optional<GuestMemoryFault> Fault = std::nullopt,
                    std::string Detail = {}) {
  return llvm::make_error<X86TranslationBlockBuilderError>(Code, GuestPC, Fault,
                                                           std::move(Detail));
}

std::optional<GuestMemoryFault>
faultDetails(const GuestInstructionFetchResult &Fetch) {
  return Fetch.Fault;
}

struct ClassifiedControl {
  bool IsBranch = false;
  bool IsConditional = false;
  bool IsCall = false;
  bool IsReturn = false;
  bool IsIndirect = false;
  bool IsOpaque = false;
  std::optional<uint64_t> DirectTarget;

  bool terminatesBlock() const {
    return IsBranch || IsCall || IsReturn || IsOpaque;
  }
};

bool isLoopBranch(unsigned InstructionId) {
  return InstructionId == X86_INS_LOOP || InstructionId == X86_INS_LOOPE ||
         InstructionId == X86_INS_LOOPNE;
}

llvm::Expected<ClassifiedControl>
classifyLiftedControl(llvm::ArrayRef<LowOp> Ops, uint64_t GuestPC) {
  ClassifiedControl Control;
  for (const LowOp &Op : Ops) {
    switch (Op.Opcode) {
    case NdOp::BRANCH:
      Control.IsBranch = true;
      if (Op.NumInputs != 0 && Op.Inputs[0].isConst())
        Control.DirectTarget = Op.Inputs[0].Offset;
      break;
    case NdOp::COND_BR:
      Control.IsBranch = true;
      Control.IsConditional = true;
      if (Op.NumInputs != 0 && Op.Inputs[0].isConst())
        Control.DirectTarget = Op.Inputs[0].Offset;
      break;
    case NdOp::INDIR_BR:
      Control.IsBranch = true;
      Control.IsIndirect = true;
      break;
    case NdOp::CALL:
      Control.IsCall = true;
      if (Op.NumInputs != 0 && Op.Inputs[0].isConst())
        Control.DirectTarget = Op.Inputs[0].Offset;
      break;
    case NdOp::INDIR_CALL:
      Control.IsCall = true;
      Control.IsIndirect = true;
      break;
    case NdOp::RETURN:
      Control.IsReturn = true;
      break;
    default:
      break;
    }
  }

  if ((Control.IsCall && (Control.IsBranch || Control.IsReturn)) ||
      (Control.IsReturn && Control.IsBranch) ||
      (Control.IsConditional && (!Control.IsBranch || Control.IsIndirect)) ||
      (Control.IsIndirect && !(Control.IsBranch || Control.IsCall)) ||
      (Control.IsOpaque &&
       (Control.IsBranch || Control.IsCall || Control.IsReturn)))
    return failure(X86TranslationBlockBuilderErrorCode::InconsistentControl,
                   GuestPC, std::nullopt,
                   "contradictory LowIR control operations");

  if ((Control.IsBranch || Control.IsCall) && !Control.IsIndirect &&
      !Control.DirectTarget)
    return failure(X86TranslationBlockBuilderErrorCode::InconsistentControl,
                   GuestPC, std::nullopt,
                   "direct control transfer has no target");
  return Control;
}

llvm::Expected<ClassifiedControl>
classifyDecodedControl(const Decoder &TheDecoder,
                       const DecodedInsn &Instruction, uint64_t GuestPC) {
  if (Instruction.Raw == nullptr)
    return failure(X86TranslationBlockBuilderErrorCode::InconsistentDecode,
                   GuestPC, std::nullopt,
                   "decoder returned no instruction metadata");

  ClassifiedControl Control;
  // Capstone classifies LOOP/LOOPE/LOOPNE only as branch-relative, not in the
  // generic jump group.  Their decoded immediate and lifted COND_BR still form
  // an ordinary conditional block terminator.
  const bool IsBranch =
      cs_insn_group(TheDecoder.getHandle(), Instruction.Raw, CS_GRP_JUMP) ||
      isLoopBranch(Instruction.Id);
  const bool IsCall =
      cs_insn_group(TheDecoder.getHandle(), Instruction.Raw, CS_GRP_CALL);
  const bool IsReturn =
      cs_insn_group(TheDecoder.getHandle(), Instruction.Raw, CS_GRP_RET);
  const unsigned ControlKindCount = static_cast<unsigned>(IsBranch) +
                                    static_cast<unsigned>(IsCall) +
                                    static_cast<unsigned>(IsReturn);
  if (ControlKindCount > 1)
    return failure(X86TranslationBlockBuilderErrorCode::InconsistentDecode,
                   GuestPC, std::nullopt,
                   "decoder assigned contradictory control groups");

  Control.IsBranch = IsBranch;
  Control.IsCall = IsCall;
  Control.IsReturn = IsReturn;
  Control.IsConditional = IsBranch && Instruction.Id != X86_INS_JMP &&
                          Instruction.Id != X86_INS_JMPABS &&
                          Instruction.Id != X86_INS_LJMP;

  if (IsCall) {
    const va_t Target = TheDecoder.directCallTarget(Instruction);
    if (Target == InvalidVA)
      Control.IsIndirect = true;
    else
      Control.DirectTarget = Target;
  } else if (IsBranch) {
    if (Instruction.Raw->detail == nullptr)
      return failure(X86TranslationBlockBuilderErrorCode::InconsistentDecode,
                     GuestPC, std::nullopt,
                     "branch instruction has no operand metadata");
    const cs_x86 &X86 = Instruction.Raw->detail->x86;
    if (X86.op_count == 0)
      return failure(X86TranslationBlockBuilderErrorCode::InconsistentDecode,
                     GuestPC, std::nullopt,
                     "branch instruction has no target operand");
    if (X86.operands[0].type == X86_OP_IMM)
      Control.DirectTarget = static_cast<uint64_t>(X86.operands[0].imm);
    else
      Control.IsIndirect = true;
  }

  if (!Control.terminatesBlock() &&
      TheDecoder.isFunctionTerminator(Instruction))
    Control.IsOpaque = true;
  return Control;
}

llvm::Expected<ClassifiedControl>
classifyControl(const Decoder &TheDecoder, const DecodedInsn &Instruction,
                llvm::ArrayRef<LowOp> Ops, uint64_t GuestPC) {
  llvm::Expected<ClassifiedControl> DecodedOrErr =
      classifyDecodedControl(TheDecoder, Instruction, GuestPC);
  if (!DecodedOrErr)
    return DecodedOrErr.takeError();
  llvm::Expected<ClassifiedControl> LiftedOrErr =
      classifyLiftedControl(Ops, GuestPC);
  if (!LiftedOrErr)
    return LiftedOrErr.takeError();

  const ClassifiedControl &Decoded = *DecodedOrErr;
  const ClassifiedControl &Lifted = *LiftedOrErr;
  if (!Lifted.terminatesBlock())
    return Decoded;

  const bool KindMatches = Decoded.IsBranch == Lifted.IsBranch &&
                           Decoded.IsCall == Lifted.IsCall &&
                           Decoded.IsReturn == Lifted.IsReturn &&
                           Decoded.IsOpaque == Lifted.IsOpaque;
  const bool ShapeMatches = Decoded.IsConditional == Lifted.IsConditional &&
                            Decoded.IsIndirect == Lifted.IsIndirect;
  const bool TargetMatches =
      Decoded.DirectTarget.has_value() == Lifted.DirectTarget.has_value() &&
      (!Decoded.DirectTarget || *Decoded.DirectTarget == *Lifted.DirectTarget);
  if (!KindMatches || !ShapeMatches || !TargetMatches)
    return failure(X86TranslationBlockBuilderErrorCode::InconsistentControl,
                   GuestPC, std::nullopt,
                   "decoder and LowIR control metadata disagree");
  return Decoded;
}

X86TranslationBlockBuilderErrorCode
fetchFailureCode(const GuestInstructionFetchResult &Fetch,
                 X86TranslationBlockBuilderErrorCode DefaultCode) {
  if (Fetch.Fault &&
      Fetch.Fault->Kind == RuntimeMemoryFaultKindV1::AddressOverflow)
    return X86TranslationBlockBuilderErrorCode::GuestAddressOverflow;
  return DefaultCode;
}

LowInstructionBoundary makeBoundary(uint64_t Address, uint16_t Size,
                                    uint64_t FirstOp, uint64_t OpCount,
                                    const ClassifiedControl &Control,
                                    std::optional<uint64_t> ReturnImmediate) {
  LowInstructionBoundary Boundary;
  Boundary.Address = Address;
  Boundary.Size = Size;
  Boundary.FirstOp = FirstOp;
  Boundary.OpCount = OpCount;
  Boundary.Mode = InstructionMode::Default;

  auto AddFlag = [&](bool IsSet, LowInstructionControlFlag Flag) {
    if (IsSet)
      Boundary.ControlFlags |= Flag;
  };
  AddFlag(Control.IsBranch, LowInstructionControlFlag::Branch);
  AddFlag(Control.IsConditional, LowInstructionControlFlag::Conditional);
  AddFlag(Control.IsCall, LowInstructionControlFlag::Call);
  AddFlag(Control.IsReturn, LowInstructionControlFlag::Return);
  AddFlag(Control.IsIndirect, LowInstructionControlFlag::Indirect);

  if (Control.IsOpaque) {
    Boundary.Control = LowInstructionControl::Terminator;
    Boundary.ControlFlags = LowInstructionControlFlag::Terminator;
  } else if (Control.IsBranch) {
    Boundary.Control = LowInstructionControl::Branch;
    Boundary.Immediate =
        Control.IsIndirect ? std::nullopt : Control.DirectTarget;
  } else if (Control.IsCall) {
    Boundary.Control = LowInstructionControl::Call;
    Boundary.Immediate =
        Control.IsIndirect ? std::nullopt : Control.DirectTarget;
  } else if (Control.IsReturn) {
    Boundary.Control = LowInstructionControl::Return;
    Boundary.Immediate = ReturnImmediate;
  }
  return Boundary;
}

TranslationBlockTerminatorKindV1
terminatorKind(const ClassifiedControl &Control) {
  if (Control.IsOpaque)
    return TranslationBlockTerminatorKindV1::Opaque;
  if (Control.IsReturn)
    return TranslationBlockTerminatorKindV1::Return;
  if (Control.IsCall)
    return Control.IsIndirect ? TranslationBlockTerminatorKindV1::IndirectCall
                              : TranslationBlockTerminatorKindV1::DirectCall;
  if (Control.IsIndirect)
    return TranslationBlockTerminatorKindV1::IndirectBranch;
  if (Control.IsConditional)
    return TranslationBlockTerminatorKindV1::ConditionalBranch;
  if (Control.IsBranch)
    return TranslationBlockTerminatorKindV1::DirectBranch;
  return TranslationBlockTerminatorKindV1::Invalid;
}

bool hasKnownTerminator(TranslationBlockTerminatorKindV1 Kind) {
  switch (Kind) {
  case TranslationBlockTerminatorKindV1::DirectBranch:
  case TranslationBlockTerminatorKindV1::ConditionalBranch:
  case TranslationBlockTerminatorKindV1::IndirectBranch:
  case TranslationBlockTerminatorKindV1::DirectCall:
  case TranslationBlockTerminatorKindV1::IndirectCall:
  case TranslationBlockTerminatorKindV1::Return:
  case TranslationBlockTerminatorKindV1::Opaque:
    return true;
  case TranslationBlockTerminatorKindV1::Invalid:
    return false;
  }
  return false;
}

llvm::Error invalidDescriptor(llvm::Twine Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

} // namespace

llvm::Error validateTranslationBlockDescriptorV1(
    const TranslationBlockDescriptorV1 &Block) {
  const TranslationBlockDescriptorHeaderV1 &Header = Block.Header;
  if (Header.Magic != kTranslationBlockDescriptorMagicV1 ||
      Header.Version != kTranslationBlockDescriptorVersionV1 ||
      Header.Size != kTranslationBlockDescriptorHeaderSizeV1)
    return invalidDescriptor(
        "translation block descriptor identity is invalid");
  if (!hasKnownTerminator(Header.Terminator))
    return invalidDescriptor("translation block terminator is invalid");

  constexpr uint32_t KnownFlags =
      static_cast<uint32_t>(TranslationBlockDescriptorFlagV1::HasStaticTarget) |
      static_cast<uint32_t>(
          TranslationBlockDescriptorFlagV1::HasReturnImmediate);
  if ((static_cast<uint32_t>(Header.Flags) & ~KnownFlags) != 0)
    return invalidDescriptor("translation block descriptor has unknown flags");
  if (Header.GuestInstructionCount == 0 || Header.GuestByteCount == 0 ||
      Header.GuestInstructionCount != Block.InstructionBoundaries.size() ||
      Header.GuestByteCount != Block.Bytes.size())
    return invalidDescriptor("translation block counts are inconsistent");
  if (Header.GuestByteCount >
          std::numeric_limits<uint64_t>::max() - Header.EntryPC ||
      Header.FallthroughPC != Header.EntryPC + Header.GuestByteCount)
    return invalidDescriptor("translation block guest range is invalid");

  const bool HasStaticTarget = hasTranslationBlockDescriptorFlag(
      Header.Flags, TranslationBlockDescriptorFlagV1::HasStaticTarget);
  const bool RequiresStaticTarget =
      Header.Terminator == TranslationBlockTerminatorKindV1::DirectBranch ||
      Header.Terminator ==
          TranslationBlockTerminatorKindV1::ConditionalBranch ||
      Header.Terminator == TranslationBlockTerminatorKindV1::DirectCall;
  if (HasStaticTarget != RequiresStaticTarget ||
      (!HasStaticTarget && Header.StaticTargetPC != 0))
    return invalidDescriptor("translation block static target is inconsistent");

  const bool HasReturnImmediate = hasTranslationBlockDescriptorFlag(
      Header.Flags, TranslationBlockDescriptorFlagV1::HasReturnImmediate);
  if (HasReturnImmediate &&
      (Header.Terminator != TranslationBlockTerminatorKindV1::Return ||
       Header.ReturnImmediate > std::numeric_limits<uint16_t>::max()))
    return invalidDescriptor("translation block return immediate is invalid");
  if (!HasReturnImmediate && Header.ReturnImmediate != 0)
    return invalidDescriptor(
        "translation block has an unflagged return immediate");

  uint64_t ExpectedAddress = Header.EntryPC;
  uint64_t ExpectedFirstOp = 0;
  for (size_t Index = 0; Index != Block.InstructionBoundaries.size(); ++Index) {
    const LowInstructionBoundary &Boundary = Block.InstructionBoundaries[Index];
    if (Boundary.Address != ExpectedAddress || Boundary.Size == 0 ||
        Boundary.Size > kMaximumX86InstructionSize ||
        Boundary.Size > std::numeric_limits<uint64_t>::max() - ExpectedAddress)
      return invalidDescriptor("translation block boundaries are not exact");
    if (Boundary.FirstOp != ExpectedFirstOp ||
        ExpectedFirstOp > Block.Ops.size() ||
        Boundary.OpCount > Block.Ops.size() - ExpectedFirstOp)
      return invalidDescriptor("translation block LowIR slice is invalid");
    const uint64_t OpEnd = ExpectedFirstOp + Boundary.OpCount;
    for (uint64_t OpIndex = ExpectedFirstOp; OpIndex != OpEnd; ++OpIndex)
      if (Block.Ops[static_cast<size_t>(OpIndex)].Addr != Boundary.Address)
        return invalidDescriptor("translation block LowIR operation crosses an "
                                 "instruction boundary");
    const bool IsLast = Index + 1 == Block.InstructionBoundaries.size();
    if (Boundary.Mode != InstructionMode::Default ||
        Boundary.TargetMode != LowInstructionTargetMode::Preserve)
      return invalidDescriptor(
          "translation block boundary has an invalid x86 decode mode");
    if ((!IsLast && (Boundary.Control != LowInstructionControl::None ||
                     Boundary.ControlFlags != LowInstructionControlFlag::None ||
                     Boundary.Immediate.has_value())) ||
        (IsLast && Boundary.Control == LowInstructionControl::None))
      return invalidDescriptor(
          "translation block does not stop at its first control boundary");
    ExpectedAddress += Boundary.Size;
    ExpectedFirstOp = OpEnd;
  }
  if (ExpectedAddress != Header.FallthroughPC ||
      ExpectedFirstOp != Block.Ops.size())
    return invalidDescriptor("translation block boundaries are incomplete");

  const LowInstructionBoundary &Last = Block.InstructionBoundaries.back();
  const bool IsIndirect = hasLowInstructionControlFlag(
      Last.ControlFlags, LowInstructionControlFlag::Indirect);
  const bool IsConditional = hasLowInstructionControlFlag(
      Last.ControlFlags, LowInstructionControlFlag::Conditional);
  TranslationBlockTerminatorKindV1 BoundaryKind =
      TranslationBlockTerminatorKindV1::Invalid;
  switch (Last.Control) {
  case LowInstructionControl::Branch:
    BoundaryKind = IsIndirect ? TranslationBlockTerminatorKindV1::IndirectBranch
                   : IsConditional
                       ? TranslationBlockTerminatorKindV1::ConditionalBranch
                       : TranslationBlockTerminatorKindV1::DirectBranch;
    break;
  case LowInstructionControl::Call:
    BoundaryKind = IsIndirect ? TranslationBlockTerminatorKindV1::IndirectCall
                              : TranslationBlockTerminatorKindV1::DirectCall;
    break;
  case LowInstructionControl::Return:
    BoundaryKind = TranslationBlockTerminatorKindV1::Return;
    break;
  case LowInstructionControl::Terminator:
    BoundaryKind = TranslationBlockTerminatorKindV1::Opaque;
    break;
  default:
    break;
  }
  if (BoundaryKind != Header.Terminator)
    return invalidDescriptor("translation block terminator summary disagrees");
  LowInstructionControlFlag ExpectedControlFlags =
      LowInstructionControlFlag::None;
  switch (Header.Terminator) {
  case TranslationBlockTerminatorKindV1::DirectBranch:
    ExpectedControlFlags = LowInstructionControlFlag::Branch;
    break;
  case TranslationBlockTerminatorKindV1::ConditionalBranch:
    ExpectedControlFlags = LowInstructionControlFlag::Branch |
                           LowInstructionControlFlag::Conditional;
    break;
  case TranslationBlockTerminatorKindV1::IndirectBranch:
    ExpectedControlFlags =
        LowInstructionControlFlag::Branch | LowInstructionControlFlag::Indirect;
    break;
  case TranslationBlockTerminatorKindV1::DirectCall:
    ExpectedControlFlags = LowInstructionControlFlag::Call;
    break;
  case TranslationBlockTerminatorKindV1::IndirectCall:
    ExpectedControlFlags =
        LowInstructionControlFlag::Call | LowInstructionControlFlag::Indirect;
    break;
  case TranslationBlockTerminatorKindV1::Return:
    ExpectedControlFlags = LowInstructionControlFlag::Return;
    break;
  case TranslationBlockTerminatorKindV1::Opaque:
    ExpectedControlFlags = LowInstructionControlFlag::Terminator;
    break;
  case TranslationBlockTerminatorKindV1::Invalid:
    break;
  }
  if (Last.ControlFlags != ExpectedControlFlags)
    return invalidDescriptor(
        "translation block control flags disagree with its terminator");
  if (HasStaticTarget &&
      (!Last.Immediate || *Last.Immediate != Header.StaticTargetPC))
    return invalidDescriptor(
        "translation block target disagrees with boundary");
  if (Header.Terminator == TranslationBlockTerminatorKindV1::Return &&
      (Last.Immediate.has_value() != HasReturnImmediate ||
       (Last.Immediate && *Last.Immediate != Header.ReturnImmediate)))
    return invalidDescriptor(
        "translation block return immediate disagrees with boundary");
  if (!HasStaticTarget &&
      Header.Terminator != TranslationBlockTerminatorKindV1::Return &&
      Last.Immediate)
    return invalidDescriptor(
        "translation block has an unexpected control immediate");

  if (Block.GenerationBindings.empty())
    return invalidDescriptor("translation block has no generation bindings");
  uint64_t ExpectedBindingAddress = Header.EntryPC;
  for (const GuestExecutableRangeBinding &Binding : Block.GenerationBindings) {
    if (Binding.Address != ExpectedBindingAddress || Binding.Size == 0 ||
        Binding.Size >
            std::numeric_limits<uint64_t>::max() - ExpectedBindingAddress)
      return invalidDescriptor(
          "translation block generation bindings are not exact");
    ExpectedBindingAddress += Binding.Size;
  }
  if (ExpectedBindingAddress != Header.FallthroughPC)
    return invalidDescriptor(
        "translation block generation bindings do not cover its bytes");
  return llvm::Error::success();
}

X86TranslationBlockBuilder::X86TranslationBlockBuilder(
    std::unique_ptr<Decoder> Decoder)
    : TheDecoder(std::move(Decoder)) {}

X86TranslationBlockBuilder::~X86TranslationBlockBuilder() = default;

llvm::Expected<std::unique_ptr<X86TranslationBlockBuilder>>
X86TranslationBlockBuilder::create() {
  auto DecoderInstance = std::make_unique<Decoder>();
  DecoderInstance->setStrict(true);
  if (!DecoderInstance->init(Arch::X64, InstructionMode::Default))
    return failure(
        X86TranslationBlockBuilderErrorCode::DecoderInitializationFailed, 0);
  return std::unique_ptr<X86TranslationBlockBuilder>(
      new X86TranslationBlockBuilder(std::move(DecoderInstance)));
}

llvm::Expected<TranslationBlockDescriptorV1>
X86TranslationBlockBuilder::build(GuestMemoryRuntime &Memory, uint64_t EntryPC,
                                  uint64_t InstructionBudget) {
  // A block descriptor is independently cacheable and reproducible.  Decoder
  // idiom state (get-PC, dividend setup, x87 TOP) must therefore depend only on
  // bytes in this build, never on whichever block this builder handled before.
  TheDecoder->resetX86FpuState();

  TranslationBlockDescriptorV1 Block;
  Block.Header.EntryPC = EntryPC;
  uint64_t PC = EntryPC;

  while (true) {
    if (InstructionBudget != 0 &&
        Block.InstructionBoundaries.size() >= InstructionBudget)
      return failure(
          X86TranslationBlockBuilderErrorCode::InstructionBudgetExceeded, PC,
          std::nullopt, "the next instruction was not fetched or decoded");

    std::array<uint8_t, kMaximumX86InstructionSize> Candidate{};
    DecodedInsn Instruction{};
    uint16_t InstructionSize = 0;
    for (uint64_t PrefixSize = 1; PrefixSize <= kMaximumX86InstructionSize;
         ++PrefixSize) {
      GuestInstructionFetchResult Fetch = Memory.fetchInstructionBytes(
          PC, llvm::MutableArrayRef<uint8_t>(Candidate.data(), PrefixSize));
      if (Fetch.Status != GuestMemoryAccessStatus::Completed) {
        const X86TranslationBlockBuilderErrorCode DefaultCode =
            PrefixSize == 1
                ? X86TranslationBlockBuilderErrorCode::InstructionFetchFailed
                : X86TranslationBlockBuilderErrorCode::TruncatedInstruction;
        return failure(fetchFailureCode(Fetch, DefaultCode), PC,
                       faultDetails(Fetch));
      }

      const int DecodedSize = TheDecoder->decodeOneForLift(
          Candidate.data(), static_cast<size_t>(PrefixSize), PC, Instruction);
      if (DecodedSize <= 0)
        continue;
      if (DecodedSize > static_cast<int>(PrefixSize) ||
          DecodedSize > static_cast<int>(kMaximumX86InstructionSize))
        return failure(X86TranslationBlockBuilderErrorCode::InconsistentDecode,
                       PC);
      InstructionSize = static_cast<uint16_t>(DecodedSize);
      break;
    }
    if (InstructionSize == 0)
      return failure(
          X86TranslationBlockBuilderErrorCode::UndecodableInstruction, PC);

    std::vector<uint8_t> ExactBytes(InstructionSize);
    GuestInstructionFetchResult ExactFetch =
        Memory.fetchInstructionBytes(PC, ExactBytes);
    if (ExactFetch.Status != GuestMemoryAccessStatus::Completed)
      return failure(
          fetchFailureCode(
              ExactFetch,
              X86TranslationBlockBuilderErrorCode::InstructionFetchFailed),
          PC, faultDetails(ExactFetch));

    DecodedInsn ExactInstruction{};
    const int ExactSize = TheDecoder->decodeOneForLift(
        ExactBytes.data(), ExactBytes.size(), PC, ExactInstruction);
    if (ExactSize != InstructionSize)
      return failure(X86TranslationBlockBuilderErrorCode::InconsistentDecode,
                     PC, std::nullopt,
                     "exact re-fetch changed the decoded length");

    const std::optional<uint64_t> ReturnImmediate =
        TheDecoder->returnImmediate(ExactInstruction);
    std::vector<LowOp> InstructionOps;
    try {
      TheDecoder->liftToLow(ExactInstruction, InstructionOps);
    } catch (const UnliftedInstruction &Unlifted) {
      return failure(X86TranslationBlockBuilderErrorCode::UnliftedInstruction,
                     PC, std::nullopt, Unlifted.what());
    }

    llvm::Expected<ClassifiedControl> ControlOrErr =
        classifyControl(*TheDecoder, ExactInstruction, InstructionOps, PC);
    if (!ControlOrErr)
      return ControlOrErr.takeError();
    const ClassifiedControl Control = *ControlOrErr;

    if (InstructionSize > std::numeric_limits<uint64_t>::max() - PC)
      return failure(X86TranslationBlockBuilderErrorCode::GuestAddressOverflow,
                     PC);
    const uint64_t FirstOp = Block.Ops.size();
    Block.Ops.insert(Block.Ops.end(),
                     std::make_move_iterator(InstructionOps.begin()),
                     std::make_move_iterator(InstructionOps.end()));
    Block.InstructionBoundaries.push_back(
        makeBoundary(PC, InstructionSize, FirstOp, Block.Ops.size() - FirstOp,
                     Control, ReturnImmediate));
    Block.Bytes.insert(Block.Bytes.end(), ExactBytes.begin(), ExactBytes.end());
    PC += InstructionSize;

    if (!Control.terminatesBlock())
      continue;

    Block.Header.FallthroughPC = PC;
    Block.Header.Terminator = terminatorKind(Control);
    Block.Header.GuestInstructionCount = Block.InstructionBoundaries.size();
    Block.Header.GuestByteCount = Block.Bytes.size();
    if (Control.DirectTarget) {
      Block.Header.Flags |= TranslationBlockDescriptorFlagV1::HasStaticTarget;
      Block.Header.StaticTargetPC = *Control.DirectTarget;
    }
    if (Control.IsReturn && ReturnImmediate) {
      Block.Header.Flags |=
          TranslationBlockDescriptorFlagV1::HasReturnImmediate;
      Block.Header.ReturnImmediate = *ReturnImmediate;
    }
    break;
  }

  std::vector<uint8_t> FinalBytes(Block.Bytes.size());
  GuestInstructionFetchResult FinalFetch =
      Memory.fetchInstructionBytes(EntryPC, FinalBytes);
  if (FinalFetch.Status != GuestMemoryAccessStatus::Completed)
    return failure(
        fetchFailureCode(
            FinalFetch,
            X86TranslationBlockBuilderErrorCode::InstructionFetchFailed),
        EntryPC, faultDetails(FinalFetch), "final exact block fetch failed");
  if (FinalBytes != Block.Bytes)
    return failure(X86TranslationBlockBuilderErrorCode::ExecutableBytesChanged,
                   EntryPC);
  Block.Bytes = std::move(FinalBytes);
  Block.GenerationBindings = std::move(FinalFetch.Bindings);

  if (llvm::Error Error = validateTranslationBlockDescriptorV1(Block))
    return failure(X86TranslationBlockBuilderErrorCode::InvalidDescriptor,
                   EntryPC, std::nullopt, llvm::toString(std::move(Error)));
  return Block;
}

} // namespace neverd::translate
