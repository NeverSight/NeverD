//===- COFFNativeEHProvenance.h - Checked native EH anchors ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_COFFNATIVEEHPROVENANCE_H
#define NEVERD_COFFNATIVEEHPROVENANCE_H
#include "neverd/Common.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <optional>
namespace neverd::coff_native_eh {
inline std::optional<uint64_t>
provenanceUInt(const llvm::OperandBundleUse &Bundle, unsigned Index,
               unsigned BitWidth) {
  if (Index >= Bundle.Inputs.size())
    return std::nullopt;
  const auto *Value =
      llvm::dyn_cast<llvm::ConstantInt>(Bundle.Inputs[Index].get());
  if (!Value || Value->getBitWidth() != BitWidth)
    return std::nullopt;
  return Value->getZExtValue();
}

struct NativeEHProvenance {
  windows_eh_md::NativeProvenanceModel Model =
      windows_eh_md::NativeProvenanceModel::SEH;
  windows_eh_md::NativeProvenanceRole Role =
      windows_eh_md::NativeProvenanceRole::ProtectedInvoke;
  va_t FunctionVA = 0;
  va_t SourceVA = 0;
  uint32_t Region = 0;
  uint32_t Clause = 0;
  va_t AuxVA = 0;
  uint32_t Flags = 0;
};

inline std::optional<NativeEHProvenance>
parseNativeEHProvenance(const llvm::CallInst &Anchor) {
  const llvm::Function *Callee = Anchor.getCalledFunction();
  if (!Callee || Callee->getIntrinsicID() != llvm::Intrinsic::sideeffect ||
      Anchor.countOperandBundlesOfType(windows_eh_md::ProvenanceBundle) != 1)
    return std::nullopt;
  auto Bundle = Anchor.getOperandBundle(windows_eh_md::ProvenanceBundle);
  if (!Bundle || Bundle->Inputs.size() != windows_eh_md::ProvenanceOperandCount)
    return std::nullopt;

  auto Version = provenanceUInt(*Bundle, windows_eh_md::ProvenanceVersion, 32);
  auto Model = provenanceUInt(*Bundle, windows_eh_md::ProvenanceModel, 8);
  auto Role = provenanceUInt(*Bundle, windows_eh_md::ProvenanceRole, 8);
  auto FunctionVA =
      provenanceUInt(*Bundle, windows_eh_md::ProvenanceFunctionVA, 64);
  auto SourceVA =
      provenanceUInt(*Bundle, windows_eh_md::ProvenanceSourceVA, 64);
  auto Region = provenanceUInt(*Bundle, windows_eh_md::ProvenanceRegion, 32);
  auto Clause = provenanceUInt(*Bundle, windows_eh_md::ProvenanceClause, 32);
  auto AuxVA = provenanceUInt(*Bundle, windows_eh_md::ProvenanceAuxVA, 64);
  auto Flags = provenanceUInt(*Bundle, windows_eh_md::ProvenanceFlags, 32);
  if (!Version || *Version != windows_eh_md::ProvenanceSchemaVersion ||
      !Model || !Role || !FunctionVA || !SourceVA || !Region || !Clause ||
      !AuxVA || !Flags)
    return std::nullopt;
  if (*Model !=
          static_cast<unsigned>(windows_eh_md::NativeProvenanceModel::SEH) &&
      *Model !=
          static_cast<unsigned>(windows_eh_md::NativeProvenanceModel::CxxFH3) &&
      *Model !=
          static_cast<unsigned>(windows_eh_md::NativeProvenanceModel::CxxFH4) &&
      *Model != static_cast<unsigned>(
                    windows_eh_md::NativeProvenanceModel::X86RegistrationSEH) &&
      *Model != static_cast<unsigned>(
                    windows_eh_md::NativeProvenanceModel::X86RegistrationCxx))
    return std::nullopt;
  if (*Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::ProtectedInvoke) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::RegionDispatch) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::HandlerTarget) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::RangeEnter) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::RangeExit) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::RangeEnterTarget) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::RangeExitTarget) &&
      *Role !=
          static_cast<unsigned>(
              windows_eh_md::NativeProvenanceRole::RegistrationChainAccess) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::RegistrationCallback) &&
      *Role !=
          static_cast<unsigned>(
              windows_eh_md::NativeProvenanceRole::RegistrationDynamicCall) &&
      *Role != static_cast<unsigned>(
                   windows_eh_md::NativeProvenanceRole::LocalUnwindHandlerCall))
    return std::nullopt;

  return NativeEHProvenance{
      static_cast<windows_eh_md::NativeProvenanceModel>(*Model),
      static_cast<windows_eh_md::NativeProvenanceRole>(*Role),
      *FunctionVA,
      *SourceVA,
      static_cast<uint32_t>(*Region),
      static_cast<uint32_t>(*Clause),
      *AuxVA,
      static_cast<uint32_t>(*Flags)};
}

} // namespace neverd::coff_native_eh
#endif
