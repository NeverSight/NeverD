#include "DarwinSourceDeclarations.h"

#include "DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/ObjC/ObjCBlocks.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/ObjC/ObjCFormattedCalls.h"

#include <algorithm>
#include <map>

namespace neverd {
namespace {
struct Declaration {
  const char *Name;
  const char *AArch64;
  const char *X64;
  const char *AArch64Modules;
  const char *X64Modules;
};
constexpr Declaration Declarations[] = {
#include "DarwinSourceDeclarations.inc"
};

struct DataDeclaration {
  const char *Name;
  const char *AArch64Modules;
  const char *X64Modules;
};
constexpr DataDeclaration DataDeclarations[] = {
#include "DarwinSourceDataDeclarations.inc"
};

using Index =
    std::map<std::string, std::optional<SourceFunctionTypeHint>, std::less<>>;
Index signatures(Arch Architecture) {
  Index Result;
  for (const auto &D : Declarations) {
    const char *Encoding = Architecture == Arch::AArch64 ? D.AArch64 : D.X64;
    auto Hint = Encoding ? parseObjCFunctionEncoding(Encoding) : std::nullopt;
    std::string Diagnostic;
    if (Hint) {
      Hint->Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
      if (!assignDarwinFixedSourceABI(*Hint, Architecture, Diagnostic))
        Hint.reset();
    }
    auto [It, Added] = Result.try_emplace(D.Name, Hint);
    // C functions have one complete declaration per linker identity. An
    // alternative declaration is not resolved by visitation order.
    if (!Added)
      It->second.reset();
  }
  return Result;
}
} // namespace

std::optional<SourceCallTypeHint>
darwinDeclaredSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  auto Import = darwinRuntimeImport(Image, ImportSlot);
  const auto Bind = Image.DyldBindSlots.find(ImportSlot);
  if (!Import || !Import->consume_front("_") ||
      Bind == Image.DyldBindSlots.end() ||
      libc::isReturnsTwiceFunction(Import->str()))
    return std::nullopt;
  const auto D = std::lower_bound(
      std::begin(Declarations), std::end(Declarations), *Import,
      [](const Declaration &D, llvm::StringRef Name) { return D.Name < Name; });
  if (D == std::end(Declarations) || D->Name != *Import ||
      !darwinExportModuleMatches(Image.Arch == Arch::AArch64 ? D->AArch64Modules
                                                             : D->X64Modules,
                                 Bind->second.Module))
    return std::nullopt;
  static const auto Arm = signatures(Arch::AArch64);
  static const auto Intel = signatures(Arch::X64);
  const auto &Index = Image.Arch == Arch::AArch64 ? Arm : Intel;
  const auto Found = Index.find(Import->str());
  if (Found == Index.end() || !Found->second)
    return std::nullopt;
  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Import->str();
  Result.Signature = *Found->second;
  return Result;
}

std::optional<SourceCallTypeHint>
darwinDeclaredSourceGlobalAddressHint(const BinaryImage &Image,
                                      va_t ImportSlot) {
  auto Import = darwinRuntimeImport(Image, ImportSlot);
  const auto Bind = Image.DyldBindSlots.find(ImportSlot);
  if (!Import || !Import->consume_front("_") ||
      Bind == Image.DyldBindSlots.end())
    return std::nullopt;
  const auto D = std::lower_bound(
      std::begin(DataDeclarations), std::end(DataDeclarations), *Import,
      [](const DataDeclaration &D, llvm::StringRef Name) {
        return D.Name < Name;
      });
  if (D == std::end(DataDeclarations) || D->Name != *Import ||
      !darwinExportModuleMatches(Image.Arch == Arch::AArch64 ? D->AArch64Modules
                                                             : D->X64Modules,
                                 Bind->second.Module))
    return std::nullopt;
  // Only non-TLS external storage with a declaration common to both platform
  // profiles is eligible. Bind the address; subsequent loads and stores still
  // access the real runtime object, without assuming its value or layout.
  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeGlobalAddress;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Import->str();
  Result.Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
  Result.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Result.Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}

std::optional<DarwinBlockParameterContract>
darwinBlockParameterContract(const BinaryImage &Image, va_t ImportSlot,
                             unsigned Parameter) {
  const auto Call = darwinDeclaredSourceCallHint(Image, ImportSlot);
  const auto Bind = Image.DyldBindSlots.find(ImportSlot);
  if (!Call || Bind == Image.DyldBindSlots.end() ||
      Parameter >= Call->Signature.Parameters.size())
    return std::nullopt;
  static constexpr struct {
    const char *Name;
    const char *AArch64;
    const char *X64;
    unsigned Parameter;
    const char *AArch64Callback;
    const char *X64Callback;
    const char *AArch64Modules;
    const char *X64Modules;
    DarwinBlockParameterContract::Lifetime Storage;
  } Declarations[] = {
#include "DarwinBlockDeclarations.inc"
  };
  std::optional<DarwinBlockParameterContract> Result;
  for (const auto &D : Declarations) {
    if (D.Name != Call->TargetName || D.Parameter != Parameter)
      continue;
    if (Result || !darwinExportModuleMatches(Image.Arch == Arch::AArch64
                                                 ? D.AArch64Modules
                                                 : D.X64Modules,
                                             Bind->second.Module))
      return std::nullopt;
    const auto Parent = parseObjCFunctionEncoding(
        Image.Arch == Arch::AArch64 ? D.AArch64 : D.X64);
    std::string Error;
    auto Callback = parseObjCBlockSignature(
        Image.Arch == Arch::AArch64 ? D.AArch64Callback : D.X64Callback,
        Image.Arch, Error);
    if (!Parent || !Callback || Callback->Parameters.empty() ||
        Callback->Parameters[0].Type->Kind != NdTypeKind::Ptr ||
        Parent->Parameters.size() != Call->Signature.Parameters.size() ||
        !equalSourceTypes(Parent->ReturnType, Call->Signature.ReturnType))
      return std::nullopt;
    for (size_t I = 0; I < Parent->Parameters.size(); ++I)
      if (!equalSourceTypes(Parent->Parameters[I].Type,
                            Call->Signature.Parameters[I].Type))
        return std::nullopt;
    if (Parent->Parameters[Parameter].Type->Kind != NdTypeKind::Ptr)
      return std::nullopt;
    Result = DarwinBlockParameterContract{std::move(*Callback), D.Storage};
  }
  return Result;
}

std::optional<SourceFunctionTypeHint>
darwinNonEscapingBlockSignature(const BinaryImage &Image, va_t ImportSlot,
                                unsigned Parameter) {
  auto Contract = darwinBlockParameterContract(Image, ImportSlot, Parameter);
  if (!Contract ||
      Contract->Storage != DarwinBlockParameterContract::Lifetime::NonEscaping)
    return std::nullopt;
  return std::move(Contract->Signature);
}

std::optional<DarwinFormatDeclaration>
darwinRuntimeFormatDeclaration(const BinaryImage &Image, va_t ImportSlot) {
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  const auto Bind = Image.DyldBindSlots.find(ImportSlot);
  if (!Import || !Import->starts_with("_") || Bind == Image.DyldBindSlots.end())
    return std::nullopt;
  static constexpr struct {
    const char *Name;
    const char *AArch64;
    const char *X64;
    unsigned FormatParameter;
    unsigned FixedCount;
    const char *AArch64Modules;
    const char *X64Modules;
  } Formats[] = {
#include "DarwinFormatDeclarations.inc"
  };
  std::optional<DarwinFormatDeclaration> Result;
  for (const auto &D : Formats) {
    if (D.Name != Import->drop_front())
      continue;
    if (Result || !darwinExportModuleMatches(Image.Arch == Arch::AArch64
                                                 ? D.AArch64Modules
                                                 : D.X64Modules,
                                             Bind->second.Module))
      return std::nullopt;
    auto Signature = parseObjCFunctionEncoding(
        Image.Arch == Arch::AArch64 ? D.AArch64 : D.X64);
    std::string Error;
    if (!Signature || Signature->Parameters.size() != D.FixedCount ||
        D.FormatParameter >= D.FixedCount ||
        Signature->Parameters[D.FormatParameter].Type->Kind != NdTypeKind::Ptr)
      return std::nullopt;
    Signature->Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
    if (!assignDarwinScalarSourceABI(*Signature, Image.Arch, Error))
      return std::nullopt;
    Result = DarwinFormatDeclaration{std::move(*Signature), D.Name,
                                     D.FormatParameter};
  }
  return Result;
}

std::optional<SourceCallTypeHint>
darwinFormattedSourceCallHint(const BinaryImage &Image, va_t ImportSlot,
                              va_t FormatAddress) {
  auto Declaration = darwinRuntimeFormatDeclaration(Image, ImportSlot);
  if (!Declaration)
    return std::nullopt;
  SourceCallTypeHint Call;
  Call.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Call.TargetAddress = ImportSlot;
  Call.TargetName = Declaration->Name;
  Call.Signature = std::move(Declaration->Signature);
  return bindObjCFormatArguments(Image, std::move(Call),
                                 Declaration->FormatParameter, FormatAddress);
}
} // namespace neverd
