//===- InterpreterLLVMRefinement.cpp - Native-to-LLVM proof chain ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/analysis/arch/x86_64/InterpreterLLVMRefinement.h"

#include "X64UserFlags.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

namespace neverd::analysis {
namespace {
using Profile = detail::X64UserFlags;

std::string digest(const InterpreterLLVMRefinementCertificate &C) {
  llvm::SHA256 Hash;
  Hash.update("neverd-native-llvm-refinement-v1");
  const auto Number = [&](uint64_t N) {
    uint8_t Bytes[8];
    for (unsigned I = 0; I != 8; ++I)
      Bytes[I] = static_cast<uint8_t>(N >> (I * 8));
    Hash.update(Bytes);
  };
  const auto Text = [&](llvm::StringRef S) {
    Number(S.size());
    Hash.update(S);
  };
  Number(C.SchemaVersion);
  Number(static_cast<unsigned>(C.Profile));
  Number(C.FlagsSemanticsVersion);
  Text(C.LLVMIRDigest);
  Text(C.FunctionName);
  Text(C.Native.InputDigest);
  Text(C.LLVM.InputDigest);
  Number(C.Limits.MaxIRBytes);
  Number(C.Limits.MaxMachineStateOperations);
  Number(C.Limits.MaxPreparationItems);
  Number(C.Limits.LLVMModel.MaxInputItems);
  Number(C.Limits.LLVMModel.MaxBlocks);
  Number(C.Limits.LLVMModel.MaxOperations);
  Number(C.Limits.LLVMModel.MaxWork);
  // Native and LLVM proof limits, including plans and witness, are already
  // bound by their freshly computed receipt digests.
  return llvm::toHex(Hash.final());
}
} // namespace

InterpreterLLVMRefinementResult checkBinaryLLVMRefinement(
    const BinaryImage &Image, va_t Entry, const SpecializationOptions &Options,
    const LowFunc &Residual, llvm::StringRef LLVMIR,
    llvm::StringRef FunctionName, const LowIRIndependenceFrame &Frame,
    const InterpreterLLVMRefinementPlans &Plans, LowIRRefinementWitness Witness,
    const InterpreterLLVMRefinementLimits &Limits,
    const InterpreterLLVMRefinementPreservation &Preservation,
    const InterpreterLLVMNativeCollection &Collection) {
  InterpreterLLVMRefinementResult Result;
  if (Options.ExternalStoresDisjointEntryFrame) {
    Result.Stage = InterpreterLLVMRefinementStage::Native;
    Result.Native.Proof.Status = LowIRRefinementStatus::Unsupported;
    Result.Diagnostic = "native proof does not support an external-store "
                        "frame separation domain";
    Result.Native.Proof.Diagnostic = Result.Diagnostic;
    return Result;
  }
  if (!Options.ExplicitMachineState || !Options.NormalNonfaultingExecution ||
      !Options.X64CetDisabled ||
      Options.X64FlagsProfile !=
          InterpreterMachineStateProfile::UserX64NoFaultV1 ||
      Options.ByteOrder != llvm::endianness::little) {
    Result.Diagnostic = "native-to-LLVM proof requires the explicit "
                        "nonfaulting, CET-disabled x64 flags profile";
    return Result;
  }
  if (Options.EntryConstants.size() >
      Limits.NativeProof.Execution.MaxInstructions) {
    Result.Diagnostic = "native entry metadata budget exhausted";
    return Result;
  }
  auto Models = prepareInterpreterLLVMRefinement(Residual, LLVMIR, FunctionName,
                                                 Frame, Limits, Preservation);
  if (!Models) {
    Result.Diagnostic = llvm::toString(Models.takeError());
    return Result;
  }
  LowIRIndependenceContract NativeContract;
  NativeContract.X64FlagsProfile = Options.X64FlagsProfile;
  NativeContract.Frame = Frame;
  for (const auto &C : Options.EntryConstants)
    NativeContract.EntryConstants.push_back({C.Location, C.Value});
  for (unsigned I = 0; I != 16; ++I)
    NativeContract.ReturnRegisters.push_back({I * 8, 8});
  for (auto [Offset, Bit] : Profile::Flags) {
    (void)Bit;
    NativeContract.ReturnRegisters.push_back({Offset, 1});
  }
  NativeContract.PreservedRegisters = Models->Preservation.ModeledRegisters;
  NativeContract.NativePreservedState = Models->Preservation.NativeState;
  NativeContract.PreservedFrameRanges = {{0, 8}};
  NativeContract.RetainUnauditedNativeBoundaries =
      Collection.RetainUnauditedNativeBoundaries;
  NativeContract.DeferNativeConditionalEdges =
      Collection.DeferNativeConditionalEdges;

  Result.Stage = InterpreterLLVMRefinementStage::Native;
  Result.Native = Plans.Native
                      ? checkBinaryLowIRLoopRefinement(
                            Image, Entry, Options, Residual, NativeContract,
                            *Plans.Native, Witness, Limits.NativeProof)
                      : checkBinaryLowIRRefinement(Image, Entry, Options,
                                                   Residual, NativeContract,
                                                   Witness, Limits.NativeProof);
  if (!Result.Native.proved()) {
    Result.Diagnostic = Result.Native.Proof.Diagnostic;
    return Result;
  }
  Result.Stage = InterpreterLLVMRefinementStage::LLVM;
  // Both generated models are deterministic. The selected native undefined
  // witness belongs only to the native premise, not to LLVM poison handling.
  Result.LLVM =
      Plans.LLVM ? checkLowIRLoopRefinement(
                       Models->Residual.Function, Models->Residual.Instructions,
                       Models->LLVM.Function, Models->Contract, *Plans.LLVM,
                       LowIRRefinementWitness::LiftedBits, Limits.LLVMProof)
                 : checkLowIRRefinement(
                       Models->Residual.Function, Models->Residual.Instructions,
                       Models->LLVM.Function, Models->Contract,
                       LowIRRefinementWitness::LiftedBits, Limits.LLVMProof);
  if (!Result.LLVM.proved()) {
    Result.Diagnostic = Result.LLVM.Diagnostic;
    return Result;
  }
  InterpreterLLVMRefinementCertificate Certificate;
  Certificate.LLVMIRDigest = Models->LLVMIRDigest;
  Certificate.FunctionName = Models->FunctionName;
  Certificate.FlagsSemanticsVersion = Profile::SemanticsVersion;
  Certificate.Limits = Limits;
  Certificate.Native = *Result.Native.Certificate;
  Certificate.LLVM = *Result.LLVM.Certificate;
  Certificate.InputDigest = digest(Certificate);
  Result.Certificate = std::move(Certificate);
  Result.Stage = InterpreterLLVMRefinementStage::Complete;
  return Result;
}
} // namespace neverd::analysis
