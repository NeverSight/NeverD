//===- CodeGenMIPS.cpp - MIPS ELF object target selection -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Carry the input ELF's ISA and calling convention into LLVM's Mips backend.
//===----------------------------------------------------------------------===//

#include "../CodeGenDetail.h"

#include "neverd/ArchSupport.h"
#include "neverd/loader/ELF/MIPSImageMetadata.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MipsABIFlags.h"
#include "llvm/TargetParser/SubtargetFeature.h"

namespace neverd {

llvm::Expected<MIPSELFCodegenTarget> configureMIPSELFCodegen(
    llvm::Module &Module, Arch Target, const llvm::Target &Backend,
    const MIPSImageMetadata *Profile, llvm::Reloc::Model DefaultRelocation) {
  auto Unsupported = [](const llvm::Twine &Detail) {
    return llvm::createStringError(llvm::errc::not_supported,
                                   "MIPS ELF code generation: " + Detail);
  };
  if (Profile && Profile->architecture(archLittleEndian(Target)) != Target)
    return Unsupported("input ABI disagrees with the requested architecture");

  const unsigned HistoricalISA =
      Profile ? Profile->HistoricalISA : (isMIPS64O32(Target) ? 3 : 0);
#ifndef LLVM_NEVERD_MIPS_ELF_REWRITE
  // The old package uses n64 argument slots for GP64/o32 and rejects MIPS V.
  if (HistoricalISA == 5 || isMIPS64O32(Target))
    return Unsupported(
        "this ISA/ABI requires the updated NeverSight LLVM MIPS backend");
#endif
  const unsigned Revision = Profile ? Profile->ISARevision : 2;
  std::string CPU = HistoricalISA      ? "mips" + std::to_string(HistoricalISA)
                    : isMIPS64(Target) ? "mips64"
                                       : "mips32";
  if (!HistoricalISA && Revision != 1)
    CPU += "r" + std::to_string(Revision);

  const llvm::Reloc::Model Relocation =
      Profile
          ? ((Profile->ELFFlags & llvm::ELF::EF_MIPS_PIC) ? llvm::Reloc::PIC_
                                                          : llvm::Reloc::Static)
          : DefaultRelocation;
  llvm::SubtargetFeatures Features;
  if (Profile) {
    using namespace llvm::Mips;
    if (!(Profile->ELFFlags &
          (llvm::ELF::EF_MIPS_PIC | llvm::ELF::EF_MIPS_CPIC)))
      Features.AddFeature("noabicalls");
    switch (Profile->FPABI.value_or(Val_GNU_MIPS_ABI_FP_ANY)) {
    case Val_GNU_MIPS_ABI_FP_ANY:
      if (Profile->ELFFlags & llvm::ELF::EF_MIPS_FP64)
        return Unsupported("unspecified FP ABI with EF_MIPS_FP64");
      break;
    case Val_GNU_MIPS_ABI_FP_DOUBLE:
      if (isMIPSO32(Target) && Profile->ISARevision == 6)
        return Unsupported("Release 6 requires 64-bit floating registers");
      Features.AddFeature("fp64", !isMIPSO32(Target));
      break;
    case Val_GNU_MIPS_ABI_FP_SINGLE:
      Features.AddFeature("single-float");
      break;
    case Val_GNU_MIPS_ABI_FP_SOFT:
      Features.AddFeature("soft-float");
      break;
    case Val_GNU_MIPS_ABI_FP_XX:
      if (!isMIPSO32(Target) || Profile->ISARevision == 6)
        return Unsupported("FPXX requires a pre-R6 o32 target");
      Features.AddFeature("fp64", false);
      Features.AddFeature("fpxx");
      Features.AddFeature("nooddspreg");
      break;
    case Val_GNU_MIPS_ABI_FP_64:
    case Val_GNU_MIPS_ABI_FP_64A:
      if (!isMIPSO32(Target))
        return Unsupported("the selected FP ABI requires o32");
      Features.AddFeature("fp64");
      if (Profile->FPABI == Val_GNU_MIPS_ABI_FP_64A)
        Features.AddFeature("nooddspreg");
      break;
    default:
      return Unsupported("the LLVM backend cannot emit this FP ABI");
    }
    if (Profile->ELFFlags & llvm::ELF::EF_MIPS_NAN2008)
      Features.AddFeature("nan2008");
    if (Profile->HasMSA)
      Features.AddFeature("msa");
    if (Profile->HasDSPR2)
      Features.AddFeature("dspr2");
    else if (Profile->HasDSP)
      Features.AddFeature("dsp");
    if (Profile->HasMT)
      Features.AddFeature("mt");
  }

  const std::string BaseFeatures = Features.getString();
  std::unique_ptr<llvm::MCSubtargetInfo> Subtarget(
      Backend.createMCSubtargetInfo(Module.getTargetTriple(), CPU,
                                    BaseFeatures));
  if (!Subtarget || !Subtarget->isCPUStringValid(CPU))
    return Unsupported("unrecognized ISA " + CPU);

  // Ask LLVM for the same ordered and implied features its instruction
  // selector uses. Invalid ABI combinations otherwise terminate the process.
  auto Validate = [&]() -> llvm::Error {
    if (Subtarget->checkFeatures("+gp64") != isMIPS64(Target))
      return Unsupported("GPR width disagrees with the ELF target");
    if (Revision == 1 && !HistoricalISA && !isMIPS64(Target) &&
        Subtarget->checkFeatures("+fp64"))
      return Unsupported("MIPS32 Release 1 requires 32-bit floating registers");
    if (Revision == 1 && !HistoricalISA &&
        (Subtarget->checkFeatures("+abs2008") ||
         Subtarget->checkFeatures("+use-indirect-jump-hazard")))
      return Unsupported("function requires a later MIPS ISA release");
    if (Subtarget->checkFeatures("+msa,-fp64"))
      return Unsupported("MSA requires 64-bit floating registers");
    if (!isMIPSO32(Target) && (Subtarget->checkFeatures("+fpxx") ||
                               Subtarget->checkFeatures("+nooddspreg")))
      return Unsupported("FPXX and restricted odd FPRs require o32");
    if (Relocation == llvm::Reloc::PIC_ &&
        Subtarget->checkFeatures("+noabicalls"))
      return Unsupported("PIC requires ABI calls");
    if (Revision == 6 &&
        !Subtarget->checkFeatures("+mips32r6,+fp64,+nan2008,+abs2008,-dsp"))
      return Unsupported("function features conflict with the Release 6 ISA");
    return llvm::Error::success();
  };
  if (auto Error = Validate())
    return Error;
  // LLVM selects the calling convention per function. Its ELF streamer can
  // therefore describe a soft-float object even when the input passes doubles
  // in $f12/$f14. Preserve the resolved ABI and ISA across local overrides.
  llvm::FeatureBitset ProfileFeatures;
  if (Profile) {
    for (const auto &Feature : Subtarget->getAllProcessorFeatures()) {
      if (Subtarget->isCPUStringValid(Feature.Key) ||
          llvm::StringSwitch<bool>(Feature.Key)
              .Cases({"fp64", "fpxx", "single-float", "soft-float",
                      "nooddspreg", "nan2008", "noabicalls", "ptr64"},
                     true)
              .Default(false))
        ProfileFeatures.set(Feature.Value);
    }
  }
  const auto InputFeatures = Subtarget->getFeatureBits() & ProfileFeatures;
  const bool InputSoftFloat = Subtarget->checkFeatures("+soft-float");
  llvm::SmallVector<std::pair<llvm::Function *, std::string>> Updates;
  for (auto &Function : Module) {
    if (Function.isDeclaration())
      continue;
    auto LocalCPU = Function.getFnAttribute("target-cpu");
    if (LocalCPU.isStringAttribute() && LocalCPU.getValueAsString() != CPU)
      return Unsupported("function CPU disagrees with the ELF target");
    const auto Local = Function.getFnAttribute("target-features");
    llvm::StringRef Effective = BaseFeatures;
    if (Local.isStringAttribute()) {
      const llvm::StringRef Overrides = Local.getValueAsString();
      // A function-local list replaces the machine list. Preserve the ELF ABI
      // beneath local restrictions such as -msa, including on repeated calls.
      Effective = Overrides;
      if (!BaseFeatures.empty() && Overrides != BaseFeatures &&
          !Overrides.starts_with(BaseFeatures + ",")) {
        auto &Update = Updates.emplace_back(
            &Function, Overrides.empty()
                           ? BaseFeatures
                           : BaseFeatures + "," + Overrides.str());
        Effective = Update.second;
      }
    }
    if (Effective != Subtarget->getFeatureString()) {
      llvm::StringRef Remaining = Effective;
      while (!Remaining.empty()) {
        auto [Token, Rest] = Remaining.split(',');
        auto Name = Token.substr(1);
        auto Entries = Subtarget->getAllProcessorFeatures();
        auto Entry = llvm::lower_bound(Entries, Name);
        if (!(Token.starts_with("+") || Token.starts_with("-")) ||
            Entry == Entries.end() || Name != Entry->Key)
          return Unsupported("unknown target feature '" + Token + "'");
        Remaining = Rest;
      }
      Subtarget->setDefaultFeatures(CPU, CPU, Effective);
      if (auto Error = Validate())
        return Error;
      if ((Subtarget->getFeatureBits() & ProfileFeatures) != InputFeatures)
        return Unsupported("function features change the input ELF ISA or ABI");
    }
    if (Profile && !InputSoftFloat &&
        Function.getFnAttribute("use-soft-float").getValueAsBool())
      return Unsupported(
          "function soft-float calling convention conflicts with "
          "the input ELF ABI");
    bool Micro = Function.hasFnAttribute("micromips") ||
                 (!Function.hasFnAttribute("nomicromips") &&
                  Subtarget->checkFeatures("+micromips"));
    bool Mips16 = Function.hasFnAttribute("mips16") ||
                  (!Function.hasFnAttribute("nomips16") &&
                   Subtarget->checkFeatures("+mips16"));
#ifndef LLVM_NEVERD_MIPS_ELF_REWRITE
    if (Mips16 || Micro)
      return Unsupported("compressed function entry modes require the updated "
                         "NeverSight LLVM MIPS backend");
#endif
    if (Micro && !isMIPSO32(Target))
      return Unsupported("the LLVM backend cannot generate microMIPS64 bodies");
    if (Mips16 && (!isMIPSO32(Target) || Revision == 6 || Micro))
      return Unsupported("MIPS16 requires pre-R6 o32 without microMIPS");
  }
  for (const auto &[Function, Value] : Updates)
    Function->addFnAttr("target-features", Value);
  return MIPSELFCodegenTarget{CPU, BaseFeatures, Relocation};
}

} // namespace neverd
