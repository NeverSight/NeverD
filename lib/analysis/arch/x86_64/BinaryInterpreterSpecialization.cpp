//===- BinaryInterpreterSpecialization.cpp - Image adapter ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/arch/x86_64/BinaryInterpreterSpecialization.h"

#include "../../core/NativeUndefinedIndependence.h"
#include "X64Recovery.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/COFF/PEFixedImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <climits>
#include <limits>
#include <map>
#include <set>
#include <system_error>

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
  std::optional<PEFixedImageView> FixedPE;
  std::string FixedPEDiagnostic;
  bool FixedPEBudgetExceeded = false;
  const bool PreserveOpaqueState;

  // Without authenticated fixed-image evidence, refuse loader-fixed bytes.
  // Width information is not normalized for every relocation kind, so inspect
  // the preceding maximum x64 scalar width as well as the read itself.
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
    if (!Owner || (FixedPE ? !FixedPE->read(Address, Bytes, Executable)
                           : touchesFixup(Address, Bytes)))
      return nullptr;
    return Owner;
  }

public:
  ImageProvider(const BinaryImage &Image, const SpecializationOptions &Options,
                bool PreserveOpaqueState = false)
      : Image(Image), Options(Options),
        PreserveOpaqueState(PreserveOpaqueState) {
    Decode.init(Arch::X64);
    Decode.setStrict(true);
    if (Image.Format == BinaryFormat::COFF && !Image.Raw.empty()) {
      auto View = PEFixedImageView::create(
          Image, {.MaxBytes = Options.MaxImagePreparationBytes,
                  .MaxRecords = Options.MaxImagePreparationRecords});
      if (View)
        FixedPE = std::move(*View);
      else
        llvm::handleAllErrors(
            View.takeError(), [&](const llvm::ErrorInfoBase &E) {
              FixedPEDiagnostic = E.message();
              FixedPEBudgetExceeded =
                  E.convertToErrorCode() ==
                  std::make_error_code(std::errc::value_too_large);
            });
    }
  }

  template <typename ResultT> bool preparationFailed(ResultT &Result) const {
    if (FixedPEDiagnostic.empty())
      return false;
    using Status = decltype(Result.Status);
    Result.Status =
        FixedPEBudgetExceeded ? Status::BudgetExceeded : Status::Unsupported;
    Result.Diagnostic = FixedPEDiagnostic;
    return true;
  }

  llvm::StringRef fixedImageDigest() const {
    return FixedPE ? llvm::StringRef(FixedPE->digest()) : llvm::StringRef();
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    if (!FixedPEDiagnostic.empty())
      return llvm::createStringError(llvm::errc::not_supported,
                                     FixedPEDiagnostic);
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
    Result.IsNativeCall = Insn.Id == X86_INS_CALL;
    Result.NativeBytes.assign(S->Data.begin() + Offset,
                              S->Data.begin() + Offset + Insn.Size);
    bool LoadedMemoryCall = false;
    try {
      // With shadow stacks explicitly disabled, RDSSP leaves its destination
      // unchanged. This instruction-specific architectural rule must precede
      // lifting: the general CET intrinsic requires a shadow-stack model.
      // INCSSP remains a strict intrinsic and is classified as a profile trap
      // below. No enabled-CET operation is treated as an ordinary instruction.
      if (Options.X64CetDisabled &&
          (Insn.Id == X86_INS_RDSSPD || Insn.Id == X86_INS_RDSSPQ)) {
        Result.ProfileProjection =
            InterpreterProfileProjection::CetDisabledReadShadowStackV1;
        Result.Ops.push_back(
            LowOp{.Opcode = NdOp::NOP, .Addr = Cursor.Address});
        // This execution-profile projection has no architecture-owned
        // undefined-output certificate, even though its LowIR is a NOP.
        Result.UndefinedEffects.Coverage = LowUndefinedCoverage::Missing;
        Result.UndefinedEffects.OpCount = Result.Ops.size();
        Result.UndefinedEffects.OperationDigest =
            lowUndefinedOperationDigest(Result.Ops);
        Result.UndefinedEffects.Diagnostic =
            "CET-disabled RDSSP requires explicit profile evidence";
      } else {
        if (Options.ExplicitMachineState && Options.X64CetDisabled)
          LoadedMemoryCall = Decode.liftX64MemoryCallToLow(
              Insn, Result.Ops, &Result.UndefinedEffects,
              PreserveOpaqueState ? &Result.PreservedState : nullptr);
        if (!LoadedMemoryCall) {
          Result.UndefinedEffects = {};
          Decode.liftToLow(Insn, Result.Ops, {}, {}, &Result.UndefinedEffects,
                           PreserveOpaqueState ? &Result.PreservedState
                                               : nullptr);
        }
        if (Options.X64CetDisabled &&
            (Insn.Id == X86_INS_INCSSPD || Insn.Id == X86_INS_INCSSPQ))
          Result.ProfileProjection = InterpreterProfileProjection::
              CetDisabledIncrementShadowStackTrapV1;
      }
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
             (Op.Opcode == NdOp::INDIR_CALL &&
              ((LoadedMemoryCall && Op.Inputs[0].isTemp()) ||
               (Op.Inputs[0].isReg() && Insn.Raw && Insn.Raw->detail &&
                Insn.Raw->detail->x86.op_count == 1 &&
                Insn.Raw->detail->x86.operands[0].type == X86_OP_REG)))))
          // The architecture's explicit target projection supplies memory
          // reads before the call. An ordinary constant IAT/GOT slot operand
          // is still an address, never permission to use it as the callee.
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
        // This physical projection pops exactly eight bytes. Do not infer
        // that width from the mnemonic when operand-size or other prefixes
        // may change the instruction's contract.
        if (Options.ExplicitMachineState && Options.X64CetDisabled &&
            !((Result.NativeBytes.size() == 1 &&
               Result.NativeBytes[0] == 0xc3) ||
              (Result.NativeBytes.size() == 3 &&
               Result.NativeBytes[0] == 0xc2)))
          return llvm::createStringError(llvm::errc::not_supported,
                                         "native stack projection requires a "
                                         "canonical 64-bit near return");
        if (auto Pop = Decode.returnImmediate(Insn);
            Pop && *Pop != 0 &&
            !(Options.ExplicitMachineState && Options.X64CetDisabled))
          return llvm::createStringError(
              llvm::errc::not_supported,
              "callee-pop returns require physical native stack semantics");
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
    // Intel SDM 253666-093, INCSSPD/INCSSPQ (3-459/460): disabled shadow
    // stacks cause #UD before any access. Keep the original intrinsic and
    // Missing sidecar. A nonfaulting proof must show this boundary unreachable.
    if (Result.ProfileProjection ==
        InterpreterProfileProjection::CetDisabledIncrementShadowStackTrapV1) {
      B.Control = LowInstructionControl::Terminator;
      B.ControlFlags = LowInstructionControlFlag::Terminator;
    }
    if (Decode.isFunctionTerminator(Insn) &&
        B.Control == LowInstructionControl::None) {
      if (Insn.Id != X86_INS_INT3 && Insn.Id != X86_INS_UD2)
        return llvm::createStringError(llvm::errc::not_supported,
                                       "unsupported machine terminator");
      // Preserve strictly lifted traps for whole-original-graph inspection.
      // This does not authorize execution or model exception resumption.
      B.Control = LowInstructionControl::Terminator;
      B.ControlFlags = LowInstructionControlFlag::Terminator;
      if (Decode.isResumableTrap(Insn))
        B.ControlFlags |= LowInstructionControlFlag::Resumable;
    }
    Result.Fallthrough = {Cursor.Address + Insn.Size, Cursor.Mode};
    if (PreserveOpaqueState &&
        Result.ProfileProjection ==
            InterpreterProfileProjection::CetDisabledReadShadowStackV1)
      x64::bindCetDisabledPreservedState(Result);
    return Result;
  }

  std::optional<SpecializationImmutableRead>
  immutableRead(va_t Address, uint16_t Bytes) override {
    if (!FixedPEDiagnostic.empty())
      return std::nullopt;
    const auto *S = immutableMapping(Address, Bytes, false);
    if (!S)
      return std::nullopt;
    const auto *Begin = S->Data.data() + (Address - S->VA);
    return SpecializationImmutableRead{
        std::vector<uint8_t>(Begin, Begin + Bytes),
        FixedPE ? "PE preferred-base snapshot; " + FixedPE->digest()
                : "file-backed read-only mapping; fixed permissions; no loader "
                  "fixup"};
  }
};
std::optional<SpecializationResult>
validateBinaryImage(const BinaryImage &Image,
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
  if (Options.FrameBaseRegister &&
      (Options.FrameBaseRegister->Offset !=
           getTargetRegInfo(Arch::X64).StackPointer ||
       Options.FrameBaseRegister->Bytes != 8)) {
    SpecializationResult Result;
    Result.Diagnostic = "binary recovery requires the entry RSP frame identity";
    return Result;
  }
  return std::nullopt;
}
} // namespace

SpecializationResult
specializeBinaryInterpreter(const BinaryImage &Image, va_t Entry,
                            const SpecializationOptions &Options) {
  if (auto Failure = validateBinaryImage(Image, Options))
    return std::move(*Failure);
  ImageProvider Provider(Image, Options);
  SpecializationResult Preparation;
  if (Provider.preparationFailed(Preparation))
    return Preparation;
  SpecializationOptions Effective = Options;
  if (!Effective.FrameBaseRegister)
    Effective.FrameBaseRegister =
        symbolic::SymRegisterRange{getTargetRegInfo(Arch::X64).StackPointer, 8};
  Effective.RequireRestoredFrameAtReturn = true;
  Effective.ExternalStoresPreserveEntryReturnSlot = true;
  auto Result = specializeInterpreter(Provider, {Entry, Image.Mode}, Effective);
  if (Result.complete())
    Result.Residual.Name = Image.getFunctionNameAt(Entry);
  return Result;
}
namespace {
std::string
binaryExecutionDigest(const BinaryImage &Image,
                      const SpecializationOptions &Options,
                      llvm::StringRef Domain, llvm::StringRef FixedImageDigest,
                      uint64_t Scope, llvm::StringRef ProofDigest,
                      llvm::ArrayRef<SpecializationInstruction> Instructions,
                      llvm::ArrayRef<SpecializationReadWitness> Reads) {
  llvm::SHA256 Hash;
  Hash.update(Domain);
  const auto Number = [&](uint64_t Value) {
    uint8_t Bytes[8];
    for (unsigned I = 0; I != 8; ++I)
      Bytes[I] = static_cast<uint8_t>(Value >> (I * 8));
    Hash.update(llvm::ArrayRef<uint8_t>(Bytes));
  };
  const auto Variable = [&](const NdVar &V) {
    Number(static_cast<unsigned>(V.Space));
    Number(V.Offset);
    Number(V.Size);
    Number(static_cast<unsigned>(V.Provenance));
    Number(V.AddressOwnerVA);
  };
  const auto Mappings = [&](va_t Address) {
    for (const auto &Mapping : Image.Segments)
      if (Mapping.contains(Address)) {
        Number(Mapping.VA);
        Number(Mapping.Size);
        Number(Mapping.FileOff);
        Number(Mapping.FileSz);
        Number(Mapping.Data.size());
        Number(static_cast<unsigned>(Mapping.Flags));
        Number(Mapping.ReadOnlyAfterRelocations);
      }
  };
  Number(static_cast<unsigned>(Image.Arch));
  Number(static_cast<unsigned>(Image.Format));
  Number(static_cast<unsigned>(Image.Mode));
  Number(Image.IsRelocatable);
  Number(Image.Base);
  Number(FixedImageDigest.size());
  Hash.update(FixedImageDigest);
  Number(Options.ExplicitMachineState);
  Number(Options.NormalNonfaultingExecution);
  Number(Options.X64CetDisabled);
  Number(Options.ExternalStoresDisjointEntryFrame);
  Number(Options.EntryFrameAlignment.has_value());
  if (Options.EntryFrameAlignment) {
    Number(Options.EntryFrameAlignment->Alignment);
    Number(Options.EntryFrameAlignment->Residue);
  }
  Number(Options.EntryFrameBounds.has_value());
  if (Options.EntryFrameBounds) {
    Number(static_cast<uint64_t>(Options.EntryFrameBounds->Begin));
    Number(static_cast<uint64_t>(Options.EntryFrameBounds->End));
  }
  Number(Options.X64FlagsProfile.has_value());
  if (Options.X64FlagsProfile)
    Number(static_cast<unsigned>(*Options.X64FlagsProfile));
  Number(static_cast<unsigned>(Image.ExceptionMetadata.ParseStatus));
  Number(Scope);
  Number(ProofDigest.size());
  Hash.update(ProofDigest);
  Number(Instructions.size());
  for (const auto &Instruction : Instructions) {
    Number(Instruction.Origin.Address);
    Number(static_cast<unsigned>(Instruction.Origin.Mode));
    Number(Instruction.Origin.Size);
    Number(Instruction.Origin.FirstOp);
    Number(Instruction.Origin.OpCount);
    Number(static_cast<unsigned>(Instruction.Origin.Control));
    Number(static_cast<unsigned>(Instruction.Origin.ControlFlags));
    Number(static_cast<unsigned>(Instruction.Origin.TargetMode));
    Number(Instruction.Origin.Immediate.has_value());
    Number(Instruction.Origin.Immediate.value_or(0));
    Hash.update(lowUndefinedOperationDigest(Instruction.Ops));
    if (Instruction.PreservedState.Audit != LowPreservedStateAudit::Missing) {
      Hash.update("neverd-original-native-preservation-v1");
      Hash.update(lowPreservedStateDigest(Instruction.PreservedState));
    }
    Hash.update(Instruction.UndefinedEffects.OperationDigest);
    const auto &Effects = Instruction.UndefinedEffects;
    Number(static_cast<unsigned>(Effects.Coverage));
    Number(Effects.OpCount);
    Number(Effects.Effects.size());
    for (const auto &Effect : Effects.Effects) {
      Number(Effect.AfterOp);
      Variable(Effect.Output);
      Number(Effect.BitOffset);
      Number(Effect.BitCount);
      Number(Effect.When.has_value());
      if (Effect.When)
        Variable(*Effect.When);
    }
    Number(Instruction.NativeBytes.size());
    Hash.update(Instruction.NativeBytes);
    Number(Instruction.Fallthrough.Address);
    Number(static_cast<unsigned>(Instruction.Fallthrough.Mode));
    Number(static_cast<unsigned>(Instruction.NativeStackControl));
    Number(Instruction.IsNativeCall);
    Number(static_cast<unsigned>(Instruction.ProfileProjection));
    // The provider has already checked uniqueness, permissions, full file
    // coverage and either absent fixups or the authenticated preferred-base
    // snapshot contract for this exact native instruction.
    Mappings(Instruction.Origin.Address);
  }
  Number(Reads.size());
  for (const auto &Read : Reads) {
    Number(Read.InstructionAddress);
    Number(static_cast<uint64_t>(Read.OpSeq));
    Number(Read.Address);
    Mappings(Read.Address);
    Number(Read.Bytes.size());
    Hash.update(Read.Bytes);
    Number(Read.Evidence.size());
    Hash.update(Read.Evidence);
  }
  return llvm::toHex(Hash.final());
}
std::optional<LowIRIndependenceResult>
prepareBinaryRelation(const BinaryImage &Image,
                      const SpecializationOptions &Options,
                      const LowIRIndependenceContract &Contract,
                      const LowIRIndependenceLimits &Limits,
                      LowIRIndependenceContract &Effective) {
  using Status = LowIRIndependenceStatus;
  LowIRIndependenceResult Result;
  const auto Fail = [&](Status S, std::string Diagnostic) {
    Result.Status = S;
    Result.Diagnostic = std::move(Diagnostic);
    return std::move(Result);
  };
  if (Image.Segments.size() > Limits.MaxInstructions ||
      Image.Relocations.size() > Limits.MaxInstructions ||
      Image.BaseRelocations.size() > Limits.MaxInstructions ||
      Image.ExceptionMetadata.Functions.size() > Limits.MaxInstructions ||
      Contract.EntryConstants.size() > Limits.MaxInstructions ||
      Options.EntryConstants.size() > Limits.MaxInstructions ||
      Contract.PreservedRegisters.size() > Limits.MaxInstructions ||
      Contract.PreservedFrameRanges.size() > Limits.MaxInstructions ||
      (Contract.Frame &&
       Contract.Frame->ExcludedAddressRanges.size() > Limits.MaxInstructions))
    return Fail(Status::BudgetExceeded,
                "original-image metadata budget exhausted");
  if (!Options.ExplicitMachineState || !Options.NormalNonfaultingExecution ||
      !Options.X64CetDisabled)
    return Fail(Status::Unsupported,
                "original-graph proof requires an explicit nonfaulting, "
                "CET-disabled machine profile");
  if (auto Failure = validateBinaryImage(Image, Options))
    return Fail(Failure->Status == SpecializationStatus::InvalidInput
                    ? Status::Invalid
                    : Status::Unsupported,
                std::move(Failure->Diagnostic));
  const auto RSP = getTargetRegInfo(Arch::X64).StackPointer;
  if (!Contract.Frame || Contract.Frame->RootRegister.Offset != RSP ||
      Contract.Frame->RootRegister.Bytes != 8 || Contract.Frame->Begin > 0 ||
      Contract.Frame->End < 8)
    return Fail(
        Status::Invalid,
        "binary proof requires an accessible entry RSP frame and return slot");
  if (Options.EntryFrameAlignment != Contract.Frame->EntryAlignment)
    return Fail(Status::Invalid,
                "proof and recovery entry alignment contracts differ");
  if (Options.EntryFrameAlignment &&
      (!Options.EntryFrameAlignment->valid() || !Options.FrameBaseRegister ||
       Options.FrameBaseRegister->Offset != RSP ||
       Options.FrameBaseRegister->Bytes != 8))
    return Fail(
        Status::Invalid,
        "entry alignment requires a valid RSP recovery root and domain");
  if (Options.ExternalStoresDisjointEntryFrame)
    return Fail(Status::Unsupported,
                "native proof does not support an external-store frame "
                "separation domain");
  if (Options.EntryFrameBounds &&
      (Options.EntryFrameBounds->Begin >= Options.EntryFrameBounds->End ||
       Options.EntryFrameBounds->Begin != Contract.Frame->Begin ||
       Options.EntryFrameBounds->End != Contract.Frame->End))
    return Fail(Status::Invalid,
                "recovery entry frame bounds do not match the proof contract");
  if (Contract.X64FlagsProfile != Options.X64FlagsProfile ||
      Contract.ByteOrder != Options.ByteOrder ||
      Contract.EntryConstants.size() != Options.EntryConstants.size())
    return Fail(Status::Invalid, "proof and recovery entry contracts differ");
  for (size_t I = 0; I != Contract.EntryConstants.size(); ++I) {
    const auto &A = Contract.EntryConstants[I];
    const auto &B = Options.EntryConstants[I];
    if (A.Location.Space != B.Location.Space ||
        A.Location.Offset != B.Location.Offset ||
        A.Location.Size != B.Location.Size || A.Value != B.Value)
      return Fail(Status::Invalid, "proof and recovery entry constants differ");
  }

  Effective = Contract;
  for (const auto &Mapping : Image.Segments) {
    if (!Mapping.Size)
      continue;
    if (Mapping.VA > InvalidVA - Mapping.Size)
      return Fail(Status::Invalid, "image mapping wraps the address space");
    Effective.Frame->ExcludedAddressRanges.push_back(
        {Mapping.VA, Mapping.VA + Mapping.Size});
  }
  // Add only uncovered bytes. Explicit overlapping user requirements remain
  // visible to the common validator rather than silently normalizing them.
  for (uint64_t I = 0; I != 8; ++I) {
    const uint64_t Byte = RSP + I;
    if (std::none_of(Contract.PreservedRegisters.begin(),
                     Contract.PreservedRegisters.end(), [&](const auto &Range) {
                       return Byte >= Range.Offset &&
                              Byte - Range.Offset < Range.Bytes;
                     }))
      Effective.PreservedRegisters.push_back({Byte, 1});
    if (std::none_of(Contract.PreservedFrameRanges.begin(),
                     Contract.PreservedFrameRanges.end(),
                     [&](const auto &Range) {
                       return Range.Offset <= static_cast<int64_t>(I) &&
                              static_cast<uint64_t>(I) -
                                      static_cast<uint64_t>(Range.Offset) <
                                  Range.Bytes;
                     }))
      Effective.PreservedFrameRanges.push_back({static_cast<int64_t>(I), 1});
  }

  return std::nullopt;
}
} // namespace

BinaryUndefinedIndependenceResult
checkBinaryUndefinedIndependence(const BinaryImage &Image, va_t Entry,
                                 const SpecializationOptions &Options,
                                 const LowIRIndependenceContract &Contract,
                                 const LowIRIndependenceLimits &Limits) {
  BinaryUndefinedIndependenceResult Result;
  LowIRIndependenceContract Effective;
  if (auto Failure =
          prepareBinaryRelation(Image, Options, Contract, Limits, Effective)) {
    Result.Proof = std::move(*Failure);
    return Result;
  }
  ImageProvider Provider(Image, Options,
                         Effective.NativePreservedState.has_value());
  if (Provider.preparationFailed(Result.Proof))
    return Result;
  auto Checked = detail::checkNativeUndefinedIndependence(
      Provider, {Entry, Image.Mode}, Effective, Limits);
  Result.Proof = std::move(Checked.Proof);
  if (Result.Proof.proved()) {
    BinaryUndefinedIndependenceCertificate Certificate;
    Certificate.InputDigest = binaryExecutionDigest(
        Image, Options, "neverd-original-native-control-independence-v10",
        Provider.fixedImageDigest(),
        static_cast<unsigned>(Result.Proof.Certificate->Scope),
        Result.Proof.Certificate->InputDigest, Checked.Instructions,
        Checked.Reads);
    Certificate.LowIR = *Result.Proof.Certificate;
    Certificate.Instructions = std::move(Checked.Instructions);
    Certificate.Reads = std::move(Checked.Reads);
    Result.Certificate = std::move(Certificate);
  }
  return Result;
}

static BinaryLowIRRefinementResult checkBinaryLowIRRefinementImpl(
    const BinaryImage &Image, va_t Entry, const SpecializationOptions &Options,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness, const LowIRRefinementLimits &Limits,
    const LowIRLoopRefinementPlan *LoopPlan) {
  BinaryLowIRRefinementResult Result;
  if (LoopPlan && Contract.AllowOverlappingNativeInstructions) {
    Result.Proof.Status = LowIRRefinementStatus::Unsupported;
    Result.Proof.Diagnostic =
        "overlapping instructions are unsupported by native loop proofs";
    return Result;
  }
  if (!Options.X64FlagsProfile) {
    Result.Proof.Status = LowIRRefinementStatus::Unsupported;
    Result.Proof.Diagnostic =
        "native refinement requires an explicit canonical flags profile";
    return Result;
  }
  LowIRIndependenceContract Effective;
  if (auto Failure = prepareBinaryRelation(Image, Options, Contract,
                                           Limits.Execution, Effective)) {
    switch (Failure->Status) {
    case LowIRIndependenceStatus::BudgetExceeded:
      Result.Proof.Status = LowIRRefinementStatus::BudgetExceeded;
      break;
    case LowIRIndependenceStatus::Unsupported:
      Result.Proof.Status = LowIRRefinementStatus::Unsupported;
      break;
    default:
      Result.Proof.Status = LowIRRefinementStatus::Invalid;
      break;
    }
    Result.Proof.Diagnostic = std::move(Failure->Diagnostic);
    return Result;
  }
  ImageProvider Provider(Image, Options,
                         Effective.NativePreservedState.has_value());
  if (Provider.preparationFailed(Result.Proof))
    return Result;
  auto Checked = detail::checkNativeLowIRRefinement(
      Provider, {Entry, Image.Mode}, Candidate, Effective, Witness, Limits,
      LoopPlan);
  Result.Proof = std::move(Checked.Proof);
  if (Result.Proof.proved()) {
    BinaryLowIRRefinementCertificate Certificate;
    Certificate.InputDigest = binaryExecutionDigest(
        Image, Options, "neverd-original-native-lowir-refinement-v5",
        Provider.fixedImageDigest(),
        static_cast<unsigned>(Result.Proof.Certificate->Scope),
        Result.Proof.Certificate->InputDigest, Checked.Instructions,
        Checked.Reads);
    Certificate.Relation = *Result.Proof.Certificate;
    Certificate.Instructions = std::move(Checked.Instructions);
    Certificate.Reads = std::move(Checked.Reads);
    Result.Certificate = std::move(Certificate);
  }
  return Result;
}

BinaryLowIRRefinementResult checkBinaryLowIRRefinement(
    const BinaryImage &Image, va_t Entry, const SpecializationOptions &Options,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness, const LowIRRefinementLimits &Limits) {
  return checkBinaryLowIRRefinementImpl(Image, Entry, Options, Candidate,
                                        Contract, Witness, Limits, nullptr);
}

BinaryLowIRRefinementResult checkBinaryLowIRLoopRefinement(
    const BinaryImage &Image, va_t Entry, const SpecializationOptions &Options,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    const LowIRLoopRefinementPlan &Plan, LowIRRefinementWitness Witness,
    const LowIRRefinementLimits &Limits) {
  return checkBinaryLowIRRefinementImpl(Image, Entry, Options, Candidate,
                                        Contract, Witness, Limits, &Plan);
}

BinaryAutomaticLowIRRefinementResult inferAndCheckBinaryLowIRLoopRefinement(
    const BinaryImage &Image, va_t Entry, const SpecializationOptions &Options,
    const SpecializationResult &Recovery,
    const LowIRIndependenceContract &Contract, LowIRRefinementWitness Witness,
    const LowIRRefinementLimits &ProofLimits,
    const LowIRLoopInferenceLimits &InferenceLimits) {
  BinaryAutomaticLowIRRefinementResult Result;
  const auto Refuse = [&](LowIRLoopInferenceStatus Status,
                          std::string Message) {
    Result.Inference.Status = Status;
    Result.Inference.Diagnostic = std::move(Message);
    Result.Refinement.Proof.Status =
        Status == LowIRLoopInferenceStatus::BudgetExceeded
            ? LowIRRefinementStatus::BudgetExceeded
        : Status == LowIRLoopInferenceStatus::Invalid
            ? LowIRRefinementStatus::Invalid
            : LowIRRefinementStatus::Unsupported;
    Result.Refinement.Proof.Diagnostic = Result.Inference.Diagnostic;
  };
  if (Contract.AllowOverlappingNativeInstructions) {
    Refuse(LowIRLoopInferenceStatus::Unsupported,
           "overlapping instructions are unsupported by native loop inference");
    return Result;
  }
  if (!Recovery.complete()) {
    Refuse(LowIRLoopInferenceStatus::Invalid,
           "automatic loop refinement requires complete recovery");
    return Result;
  }
  if (Recovery.Origins.size() > InferenceLimits.Execution.MaxInstructions ||
      Recovery.Residual.Blocks.size() >
          InferenceLimits.Execution.MaxBlockVisits) {
    Refuse(LowIRLoopInferenceStatus::BudgetExceeded,
           "loop inference origin metadata budget exhausted");
    return Result;
  }
  LowIRIndependenceContract Effective;
  if (auto Failure = prepareBinaryRelation(Image, Options, Contract,
                                           ProofLimits.Execution, Effective)) {
    Refuse(Failure->Status == LowIRIndependenceStatus::BudgetExceeded
               ? LowIRLoopInferenceStatus::BudgetExceeded
           : Failure->Status == LowIRIndependenceStatus::Invalid
               ? LowIRLoopInferenceStatus::Invalid
               : LowIRLoopInferenceStatus::Unsupported,
           Failure->Diagnostic);
    return Result;
  }
  if (!Options.X64FlagsProfile) {
    Refuse(LowIRLoopInferenceStatus::Unsupported,
           "native refinement requires an explicit canonical flags profile");
    return Result;
  }
  std::map<va_t, unsigned> NativeCounts, ResidualCounts;
  std::map<va_t, va_t> Origins;
  for (const auto &Origin : Recovery.Origins) {
    ++NativeCounts[Origin.NativeInstruction.Address];
    ++ResidualCounts[Origin.ResidualAddress];
    Origins[Origin.ResidualAddress] = Origin.NativeInstruction.Address;
  }
  std::vector<detail::NativeLoopCutpointOrigin> Eligible;
  for (const auto &B : Recovery.Residual.Blocks)
    if (ResidualCounts[B.StartAddr] == 1)
      Eligible.push_back({B.StartAddr, Origins.at(B.StartAddr),
                          NativeCounts[Origins.at(B.StartAddr)] == 1});
  if (Eligible.empty()) {
    Refuse(LowIRLoopInferenceStatus::Unsupported,
           "loop inference has no uniquely mapped residual cutpoint origins");
    return Result;
  }
  ImageProvider Provider(Image, Options,
                         Effective.NativePreservedState.has_value());
  if (Provider.preparationFailed(Result.Inference)) {
    Refuse(Result.Inference.Status, Result.Inference.Diagnostic);
    return Result;
  }
  Result.Inference = detail::inferNativeLowIRLoopRefinementPlan(
      Provider, Recovery.Residual, Effective, InferenceLimits, Eligible);
  if (!Result.Inference.inferred()) {
    Refuse(Result.Inference.Status, Result.Inference.Diagnostic);
    return Result;
  }
  for (auto &Cut : Result.Inference.Plan->Cutpoints) {
    Cut.OriginalAddress = Origins.at(Cut.CandidateAddress);
    // Recovery introduces function-local storage only in the candidate.
    // Keep its parameters and projection obligations on that side instead of
    // inventing corresponding native temporaries. The complete checker must
    // still prove the proposed machine-state relation on every arrival.
    for (auto &Input : Cut.Inputs)
      if (Input.Location.Space == LowIRLoopSpace::FunctionTemporary) {
        if (Input.Side == LowIRLoopSide::Original)
          Input.Side = LowIRLoopSide::Candidate;
        else if (Input.Side == LowIRLoopSide::OriginalPrefix)
          Input.Side = LowIRLoopSide::CandidatePrefix;
      }
    std::erase_if(Cut.OriginalState, [](const auto &Assignment) {
      return Assignment.Location.Space == LowIRLoopSpace::FunctionTemporary;
    });
  }
  Result.Refinement = checkBinaryLowIRLoopRefinement(
      Image, Entry, Options, Recovery.Residual, Contract,
      *Result.Inference.Plan, Witness, ProofLimits);
  return Result;
}

SpecializationWithIndependenceResult
specializeBinaryInterpreterWithIndependence(
    const BinaryImage &Image, va_t Entry, const SpecializationOptions &Options,
    const LowIRIndependenceContract &Contract,
    const LowIRIndependenceLimits &Limits) {
  SpecializationWithIndependenceResult Result;
  Result.Independence =
      checkBinaryUndefinedIndependence(Image, Entry, Options, Contract, Limits);
  if (!Result.Independence.proved()) {
    switch (Result.Independence.Proof.Status) {
    case LowIRIndependenceStatus::Invalid:
    case LowIRIndependenceStatus::InfeasibleEntry:
      Result.Recovery.Status = SpecializationStatus::InvalidInput;
      break;
    case LowIRIndependenceStatus::BudgetExceeded:
      Result.Recovery.Status = SpecializationStatus::BudgetExceeded;
      break;
    default:
      Result.Recovery.Status = SpecializationStatus::Unsupported;
      break;
    }
    Result.Recovery.Diagnostic = Result.Independence.Proof.Diagnostic;
    return Result;
  }
  Result.Recovery = specializeBinaryInterpreter(Image, Entry, Options);
  return Result;
}
} // namespace neverd::analysis
