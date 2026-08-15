//===- TranslationTargetMachine.cpp - Exact host code generation --------===//

#include "neverd/translate/TranslationTargetMachine.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace neverd::translate {

char TranslationTargetMachineError::ID;

TranslationTargetMachineError::TranslationTargetMachineError(
    TranslationTargetMachineErrorCode Code, std::string Detail)
    : Code(Code), Detail(std::move(Detail)) {}

void TranslationTargetMachineError::log(llvm::raw_ostream &OS) const {
  OS << "translation target machine: ";
  switch (Code) {
  case TranslationTargetMachineErrorCode::HostTargetResolutionFailed:
    OS << "host target resolution failed";
    break;
  case TranslationTargetMachineErrorCode::UnsupportedHostArchitecture:
    OS << "unsupported host architecture";
    break;
  case TranslationTargetMachineErrorCode::TargetLookupFailed:
    OS << "target lookup failed";
    break;
  case TranslationTargetMachineErrorCode::TargetCPUOrFeatureRejected:
    OS << "target CPU or feature rejected";
    break;
  case TranslationTargetMachineErrorCode::TargetMachineCreationFailed:
    OS << "target-machine creation failed";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code TranslationTargetMachineError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}

namespace {

llvm::Error failure(TranslationTargetMachineErrorCode Code,
                    llvm::StringRef Detail = {}) {
  return llvm::make_error<TranslationTargetMachineError>(Code, Detail.str());
}

llvm::Error failure(TranslationTargetMachineErrorCode Code, llvm::Error Cause) {
  return failure(Code, llvm::toString(std::move(Cause)));
}

llvm::CodeGenOptLevel codeGenerationLevel(const TranslationOptions &Options) {
  if (Options.Optimization !=
      TranslationOptimizationPolicy::ProvenSemanticAndLLVM)
    return llvm::CodeGenOptLevel::None;
  switch (Options.LLVMLevel) {
  case LLVMOptimizationLevel::O0:
    return llvm::CodeGenOptLevel::None;
  case LLVMOptimizationLevel::O1:
    return llvm::CodeGenOptLevel::Less;
  case LLVMOptimizationLevel::O2:
    return llvm::CodeGenOptLevel::Default;
  case LLVMOptimizationLevel::O3:
    return llvm::CodeGenOptLevel::Aggressive;
  }
  return llvm::CodeGenOptLevel::None;
}

void registerTranslationTargets() {
  static std::once_flag Once;
  std::call_once(Once, [] {
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
  });
}

bool isSupportedHostArchitecture(GuestArchitecture Architecture) {
  switch (Architecture) {
  case GuestArchitecture::X86_32:
  case GuestArchitecture::X86_64:
  case GuestArchitecture::ARM32:
  case GuestArchitecture::AArch64:
    return true;
  }
  return false;
}

bool hasTargetFeature(const llvm::MCSubtargetInfo &Subtarget,
                      llvm::StringRef Name) {
  return llvm::any_of(Subtarget.getAllProcessorFeatures(),
                      [&](const llvm::SubtargetFeatureKV &Feature) {
                        return Name == Feature.Key;
                      });
}

bool sameTargetRequest(const HostTarget &Left, const HostTarget &Right) {
  return Left.Kind == Right.Kind && Left.Architecture == Right.Architecture &&
         Left.Triple == Right.Triple && Left.CPU == Right.CPU &&
         Left.Features == Right.Features;
}

} // namespace

TranslationTargetMachineV1::TranslationTargetMachineV1(
    ResolvedHostTarget HostTarget, std::unique_ptr<llvm::TargetMachine> Machine,
    const TranslationOptions &Options)
    : HostTarget(std::move(HostTarget)), Machine(std::move(Machine)),
      DataLayout(this->Machine->createDataLayout()), Guest(Options.Guest),
      Mode(Options.Mode), Optimization(Options.Optimization),
      OptimizationLevel(Options.LLVMLevel) {
  this->HostTarget.bindCanonicalDataLayout(
      DataLayout.getStringRepresentation());
}

TranslationTargetMachineV1::TranslationTargetMachineV1(
    TranslationTargetMachineV1 &&) noexcept = default;

TranslationTargetMachineV1 &TranslationTargetMachineV1::operator=(
    TranslationTargetMachineV1 &&) noexcept = default;

TranslationTargetMachineV1::~TranslationTargetMachineV1() = default;

bool TranslationTargetMachineV1::matchesCodeGenerationOptions(
    const TranslationOptions &Options) const {
  return Options.Guest == Guest && Options.Mode == Mode &&
         Options.Optimization == Optimization &&
         Options.LLVMLevel == OptimizationLevel &&
         sameTargetRequest(Options.Target, HostTarget.requestedTarget());
}

llvm::Expected<TranslationTargetMachineV1>
createTranslationTargetMachineV1(const TranslationOptions &Options) {
  llvm::Expected<ResolvedHostTarget> Resolved = resolveHostTarget(Options);
  if (!Resolved)
    return failure(
        TranslationTargetMachineErrorCode::HostTargetResolutionFailed,
        Resolved.takeError());
  if (!isSupportedHostArchitecture(Resolved->architecture()))
    return failure(
        TranslationTargetMachineErrorCode::UnsupportedHostArchitecture,
        Resolved->triple());

  registerTranslationTargets();
  const llvm::Triple Triple(Resolved->triple());
  std::string LookupError;
  const llvm::Target *Target =
      llvm::TargetRegistry::lookupTarget(Triple, LookupError);
  if (!Target)
    return failure(TranslationTargetMachineErrorCode::TargetLookupFailed,
                   LookupError);

  // Probe without caller features first.  This rejects unknown names before
  // LLVM's feature parser can terminate the process or silently ignore one.
  std::unique_ptr<llvm::MCSubtargetInfo> Probe(
      Target->createMCSubtargetInfo(Triple, /*CPU=*/"", /*Features=*/""));
  if (!Probe)
    return failure(
        TranslationTargetMachineErrorCode::TargetCPUOrFeatureRejected,
        "target has no subtarget-information provider");
  if (!Resolved->cpu().empty() && !Probe->isCPUStringValid(Resolved->cpu()))
    return failure(
        TranslationTargetMachineErrorCode::TargetCPUOrFeatureRejected,
        (llvm::Twine("unknown CPU '") + Resolved->cpu() + "'").str());
  for (const std::string &Feature : Resolved->features())
    if (!hasTargetFeature(*Probe, llvm::StringRef(Feature).drop_front()))
      return failure(
          TranslationTargetMachineErrorCode::TargetCPUOrFeatureRejected,
          "unknown target feature '" + Feature + "'");

  const std::string Features = llvm::join(Resolved->features(), ",");
  llvm::TargetOptions TargetOptions;
  // None means target default.  ARM ELF defaults to EHABI and can emit a
  // cantunwind record even for nounwind functions.  DwarfCFI with no uwtable
  // attributes keeps the audited boundary free of unwind metadata.
  TargetOptions.ExceptionModel = llvm::ExceptionHandling::DwarfCFI;
  TargetOptions.EnableFastISel = false;
  TargetOptions.EnableGlobalISel = false;
  TargetOptions.GuaranteedTailCallOpt = false;
  TargetOptions.UseInitArray = false;
  TargetOptions.FunctionSections = false;
  TargetOptions.DataSections = false;
  TargetOptions.EmitAddrsig = false;
  TargetOptions.EmitCallSiteInfo = false;
  TargetOptions.EnableMachineOutliner = false;
  TargetOptions.EnableMachineFunctionSplitter = false;

  std::unique_ptr<llvm::TargetMachine> Machine(Target->createTargetMachine(
      Triple, Resolved->cpu(), Features, TargetOptions, llvm::Reloc::Static,
      llvm::CodeModel::Small, codeGenerationLevel(Options),
      Options.Mode == TranslationMode::JIT));
  if (!Machine)
    return failure(
        TranslationTargetMachineErrorCode::TargetMachineCreationFailed,
        Resolved->triple());

  return TranslationTargetMachineV1(std::move(*Resolved), std::move(Machine),
                                    Options);
}

} // namespace neverd::translate
