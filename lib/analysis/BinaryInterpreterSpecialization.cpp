//===- BinaryInterpreterSpecialization.cpp - Image adapter ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/BinaryInterpreterSpecialization.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <limits>

namespace neverd::analysis {
namespace {

bool hasUsableExceptionCoverage(const BinaryImage &Image) {
  const ExceptionInfo &Info = Image.ExceptionMetadata;
  if (Info.ParseStatus == ExceptionParseStatus::Complete)
    return true;
  // A full PE directory can be structurally complete while a different
  // function's language handler remains unknown. The provider checks every
  // covering record at each reached instruction, so only a partial result
  // accounted for by valid, localized records may be considered here.
  if (Image.Format != BinaryFormat::COFF ||
      Info.ParseStatus != ExceptionParseStatus::Partial ||
      (Info.StructuralDecode ? Info.StructuralDecode->ParseStatus !=
                                   ExceptionParseStatus::Complete
                             : !Info.Diagnostics.empty()))
    return false;
  bool HasPartialFunction = false;
  for (const ExceptionFunction &Function : Info.Functions) {
    if (!Function.CodeRange.isValid() ||
        Function.ParseStatus == ExceptionParseStatus::Malformed)
      return false;
    HasPartialFunction |= Function.ParseStatus == ExceptionParseStatus::Partial;
  }
  return HasPartialFunction;
}

class ImageProvider final : public SpecializationProvider {
  const BinaryImage &Image;
  Decoder Decode;
  const SpecializationOptions &Options;

  // The adapter deliberately refuses loader-fixed bytes. Width information is
  // not normalized for every relocation kind, so conservatively inspect the
  // preceding maximum x64 scalar relocation width as well as the read itself.
  bool touchesFixup(va_t Address, uint16_t Bytes) const {
    const va_t Begin = Address >= 7 ? Address - 7 : 0;
    const va_t End = Address + Bytes;
    for (va_t A = Begin; A < End; ++A)
      if (Image.hasRelocationProvenanceAt(A))
        return true;
    return false;
  }

  const Segment *immutableMapping(va_t Address, uint16_t Bytes,
                                  bool Executable) const {
    if (!Bytes || Address > InvalidVA - Bytes)
      return nullptr;
    const Segment *Owner = nullptr;
    for (const auto &S : Image.Segments) {
      if (!S.Size || S.VA > InvalidVA - S.Size)
        continue;
      if (S.VA >= Address + Bytes || S.VA + S.Size <= Address)
        continue;
      // Reject conflicting/overlapping mappings and writable executable code.
      if (Owner || !S.isReadable() || S.isWritable() ||
          (Executable && !S.isExecutable()) || Address < S.VA ||
          Bytes > S.Size || Address - S.VA > S.Size - Bytes ||
          Bytes > S.Data.size() || Address - S.VA > S.Data.size() - Bytes ||
          Bytes > S.FileSz || Address - S.VA > S.FileSz - Bytes)
        return nullptr;
      Owner = &S;
    }
    if (!Owner || touchesFixup(Address, Bytes))
      return nullptr;
    return Owner;
  }

public:
  ImageProvider(const BinaryImage &Image, const SpecializationOptions &Options)
      : Image(Image), Options(Options) {
    Decode.init(Arch::X64);
    Decode.setStrict(true);
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    if (Cursor.Mode != InstructionMode::Default)
      return llvm::createStringError(llvm::errc::not_supported,
                                     "unsupported instruction mode");
    const auto *S = immutableMapping(Cursor.Address, 1, true);
    if (!S)
      return llvm::createStringError(
          llvm::errc::not_supported,
          "instruction is not immutable mapped code");
    const size_t Offset = Cursor.Address - S->VA;
    const size_t Size = std::min<size_t>(15, S->Data.size() - Offset);
    DecodedInsn Insn{};
    if (!Decode.decodeOneForLift(S->Data.data() + Offset, Size, Cursor.Address,
                                 Insn) ||
        !immutableMapping(Cursor.Address, Insn.Size, true))
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "instruction decode or mapping failed");
    // Check the entire instruction against every record: an overlapping
    // fragment, including one starting inside the instruction's bytes, must
    // not hide its containing parent's handler or incomplete table parse.
    const ExceptionAddressRange InstructionRange{Cursor.Address,
                                                 Cursor.Address + Insn.Size};
    for (const auto &EH : Image.ExceptionMetadata.Functions)
      if (!Options.NormalNonfaultingExecution &&
          EH.CodeRange.overlaps(InstructionRange) &&
          (EH.hasLanguageTable() || EH.PersonalityVA || EH.HandlerDataVA ||
           EH.Personality != ExceptionPersonality::None ||
           EH.ParseStatus != ExceptionParseStatus::Complete ||
           EH.PrimaryFunctionIndex || EH.ChainedPrimaryRange))
        return llvm::createStringError(
            llvm::errc::not_supported,
            "exception edges require a recovery contract");
    // x87 and other sequence-dependent lifter state are outside this adapter's
    // integer contract; the core rejects their opaque/FP LowOps.
    Decode.resetX86FpuState();
    SpecializationInstruction Result;
    try {
      // With shadow stacks explicitly disabled, RDSSP leaves its destination
      // unchanged. This instruction-specific architectural rule must precede
      // lifting: the general CET intrinsic requires a shadow-stack model.
      // Other CET operations, including INCSSP, retain their strict rejection.
      if (Options.X64CetDisabled &&
          (Insn.Id == X86_INS_RDSSPD || Insn.Id == X86_INS_RDSSPQ))
        Result.Ops.push_back(
            LowOp{.Opcode = NdOp::NOP, .Addr = Cursor.Address});
      else
        Decode.liftToLow(Insn, Result.Ops);
    } catch (const UnliftedInstruction &Error) {
      return llvm::createStringError(llvm::errc::not_supported, "%s",
                                     Error.what());
    }
    auto &B = Result.Origin;
    B.Address = Cursor.Address;
    B.Size = Insn.Size;
    B.OpCount = Result.Ops.size();
    B.Mode = Cursor.Mode;
    B.TargetMode = Decode.controlTargetMode(Insn, Cursor.Mode);
    for (const auto &Op : Result.Ops) {
      switch (Op.Opcode) {
      case NdOp::BRANCH:
      case NdOp::COND_BR:
      case NdOp::INDIR_BR:
        B.Control = LowInstructionControl::Branch;
        B.ControlFlags |= LowInstructionControlFlag::Branch;
        if (Op.Opcode == NdOp::COND_BR)
          B.ControlFlags |= LowInstructionControlFlag::Conditional;
        if (Op.Opcode == NdOp::INDIR_BR)
          B.ControlFlags |= LowInstructionControlFlag::Indirect;
        else if (Op.NumInputs && Op.Inputs[0].isConst())
          B.Immediate = Op.Inputs[0].Offset;
        break;
      case NdOp::CALL:
      case NdOp::INDIR_CALL:
        if (Options.ExplicitMachineState && Options.X64CetDisabled &&
            Insn.Id == X86_INS_CALL && Op.NumInputs == 1 &&
            ((Op.Opcode == NdOp::CALL && Op.Inputs[0].isConst()) ||
             (Op.Opcode == NdOp::INDIR_CALL && Op.Inputs[0].isReg() &&
              Insn.Raw && Insn.Raw->detail &&
              Insn.Raw->detail->x86.op_count == 1 &&
              Insn.Raw->detail->x86.operands[0].type == X86_OP_REG)))
          // Constant-slot INDIR_CALL operands identify an IAT/GOT address,
          // not the loaded callee. Only decoded register operands certify
          // this direct-value near-call contract.
          Result.NativeStackControl = SpecializationNativeStackControl::Call;
        B.Control = LowInstructionControl::Call;
        B.ControlFlags |= LowInstructionControlFlag::Call;
        if (Op.Opcode == NdOp::INDIR_CALL)
          B.ControlFlags |= LowInstructionControlFlag::Indirect;
        else if (Op.NumInputs && Op.Inputs[0].isConst())
          B.Immediate = Op.Inputs[0].Offset;
        break;
      case NdOp::RETURN:
        if (Insn.Id != X86_INS_RET)
          return llvm::createStringError(
              llvm::errc::not_supported,
              "only ordinary near returns have a source recovery contract");
        if (auto Pop = Decode.returnImmediate(Insn); Pop && *Pop != 0)
          return llvm::createStringError(
              llvm::errc::not_supported,
              "callee-pop returns require a recovery contract");
        if (Options.ExplicitMachineState && Options.X64CetDisabled)
          Result.NativeStackControl = SpecializationNativeStackControl::Return;
        B.Control = LowInstructionControl::Return;
        B.ControlFlags |= LowInstructionControlFlag::Return;
        B.Immediate = Decode.returnImmediate(Insn);
        break;
      default:
        break;
      }
    }
    if (Decode.isFunctionTerminator(Insn) &&
        B.Control == LowInstructionControl::None)
      return llvm::createStringError(llvm::errc::not_supported,
                                     "unsupported machine terminator");
    Result.Fallthrough = {Cursor.Address + Insn.Size, Cursor.Mode};
    return Result;
  }

  std::optional<SpecializationImmutableRead>
  immutableRead(va_t Address, uint16_t Bytes) override {
    const auto *S = immutableMapping(Address, Bytes, false);
    if (!S)
      return std::nullopt;
    const auto *Begin = S->Data.data() + (Address - S->VA);
    return SpecializationImmutableRead{
        std::vector<uint8_t>(Begin, Begin + Bytes),
        "file-backed read-only mapping; fixed permissions; no loader fixup"};
  }
};
} // namespace

SpecializationResult
specializeBinaryInterpreter(const BinaryImage &Image, va_t Entry,
                            const SpecializationOptions &Options) {
  if (Image.Arch != Arch::X64 || Image.IsRelocatable ||
      Options.ByteOrder != llvm::endianness::little ||
      (Image.Format != BinaryFormat::ELF &&
       Image.Format != BinaryFormat::COFF)) {
    SpecializationResult Result;
    Result.Diagnostic =
        "interpreter specialization requires a linked x64 ELF or PE image";
    return Result;
  }
  if ((Options.NormalNonfaultingExecution || Options.X64CetDisabled) &&
      !Options.ExplicitMachineState) {
    SpecializationResult Result;
    Result.Diagnostic =
        "machine execution profile requires explicit machine-state recovery";
    return Result;
  }
  if (!hasUsableExceptionCoverage(Image)) {
    SpecializationResult Result;
    Result.Status = SpecializationStatus::Unsupported;
    Result.Diagnostic =
        "incomplete exception metadata prevents interpreter recovery";
    return Result;
  }
  // Restricted PE loads deliberately omit image-wide relocations and unwind
  // bodies outside the requested entry. An empty fixup/handler list there is
  // absence of evidence, not proof that reachable bytes are immutable or that
  // every reachable instruction has no exceptional successor.
  if (Image.Format == BinaryFormat::COFF &&
      !Image.LoadOnlyFunctionEntries.empty()) {
    SpecializationResult Result;
    Result.Status = SpecializationStatus::Unsupported;
    Result.Diagnostic = "interpreter recovery requires a full PE metadata load";
    return Result;
  }
  // COPY relocations can write a whole object, not just a scalar slot. Until
  // the loader exposes their full write footprint, file bytes cannot certify
  // an immutable read in an image carrying one.
  if (Image.Format == BinaryFormat::ELF &&
      std::any_of(Image.Relocations.begin(), Image.Relocations.end(),
                  [](const RelocationEntry &R) {
                    return R.Type == llvm::ELF::R_X86_64_COPY;
                  })) {
    SpecializationResult Result;
    Result.Status = SpecializationStatus::Unsupported;
    Result.Diagnostic = "COPY relocation write footprints are not supported";
    return Result;
  }
  ImageProvider Provider(Image, Options);
  SpecializationOptions Effective = Options;
  if (!Effective.FrameBaseRegister)
    Effective.FrameBaseRegister =
        symbolic::SymRegisterRange{getTargetRegInfo(Arch::X64).StackPointer, 8};
  if (Effective.FrameBaseRegister->Offset !=
          getTargetRegInfo(Arch::X64).StackPointer ||
      Effective.FrameBaseRegister->Bytes != 8) {
    SpecializationResult Result;
    Result.Diagnostic = "binary recovery requires the entry RSP frame identity";
    return Result;
  }
  Effective.RequireRestoredFrameAtReturn = true;
  Effective.ExternalStoresPreserveEntryReturnSlot = true;
  auto Result = specializeInterpreter(Provider, {Entry, Image.Mode}, Effective);
  if (Result.complete())
    Result.Residual.Name = Image.getFunctionNameAt(Entry);
  return Result;
}
} // namespace neverd::analysis
