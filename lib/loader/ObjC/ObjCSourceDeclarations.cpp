#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include "ObjCReceiverDeclarations.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCBlocks.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd {
namespace {
bool mergeSignature(SourceFunctionTypeHint &A,
                    const SourceFunctionTypeHint &B) {
  if (A.Parameters.size() != B.Parameters.size())
    return false;
  for (size_t I = 0; I < A.Parameters.size(); ++I)
    if (!equalSourceTypes(A.Parameters[I].Type, B.Parameters[I].Type))
      return false;
  if (equalSourceTypes(A.ReturnType, B.ReturnType))
    return true;
  // A full-width integer result carries the same 64 bits regardless of
  // signedness. Comparisons and conversions retain their own IR semantics.
  // Narrow results, floating-point and pointer identities cannot use this.
  if (!A.ReturnType || !B.ReturnType || A.ReturnType->Kind != NdTypeKind::Int ||
      B.ReturnType->Kind != NdTypeKind::Int || A.ReturnType->Size != 8 ||
      B.ReturnType->Size != 8)
    return false;
  A.ReturnType = NdType::makeInt(8, false);
  return true;
}

using SelectorSignatures = std::vector<SourceFunctionTypeHint>;
using DeclarationIndex =
    std::map<std::string, std::optional<SelectorSignatures>, std::less<>>;

struct FrameworkDeclarations {
  std::string Modules;
  DeclarationIndex Selectors;
};
using FrameworkCatalog = std::map<std::string, FrameworkDeclarations>;

FrameworkCatalog buildFrameworkDeclarations(Arch Architecture) {
  FrameworkCatalog Result;
  auto Add = [&](DeclarationIndex &Index, const char *Selector,
                 const char *Encoding) {
    if (!Encoding)
      return;
    auto Hint = parseObjCMethodEncoding(Selector, Encoding);
    std::string Diagnostic;
    if (Hint) {
      Hint->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
      if (!assignDarwinObjCSourceABI(*Hint, Architecture, Diagnostic))
        Hint.reset();
    }
    auto It = Index.try_emplace(Selector, SelectorSignatures{}).first;
    // Negative evidence survives later valid declarations and their order.
    if (!It->second)
      return;
    if (!Hint) {
      It->second.reset();
      return;
    }
    for (auto &Candidate : *It->second) {
      auto Merged = Candidate;
      if (!mergeSignature(Merged, *Hint))
        continue;
      Candidate = std::move(Merged);
      return;
    }
    It->second->push_back(std::move(*Hint));
  };
  static constexpr struct {
    const char *Selector;
    const char *AArch64;
    const char *X64;
  } Foundation[] = {
#include "ObjCFoundationDeclarations.inc"
  };
  auto &Base = Result["Foundation"];
  Base.Modules =
      "/System/Library/Frameworks/Foundation.framework/Foundation|"
      "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
  for (const auto &D : Foundation)
    Add(Base.Selectors, D.Selector,
        Architecture == Arch::AArch64 ? D.AArch64 : D.X64);
  static constexpr struct {
    const char *Framework;
    const char *Modules;
    const char *Selector;
    const char *AArch64;
    const char *X64;
  } Frameworks[] = {
#include "ObjCFrameworkDeclarations.inc"
#include "ObjCIOSFrameworkDeclarations.inc"
#include "ObjCMapKitDeclarations.inc"
  };
  for (const auto &D : Frameworks) {
    auto &Framework = Result[D.Framework];
    Framework.Modules = D.Modules;
    Add(Framework.Selectors, D.Selector,
        Architecture == Arch::AArch64 ? D.AArch64 : D.X64);
  }
  return Result;
}

const FrameworkCatalog *frameworkDeclarations(Arch Architecture) {
  // Compiler facts are immutable. Activation and binary metadata are checked
  // on every query, so another image or reload cannot retain a framework.
  if (Architecture == Arch::AArch64) {
    static const auto Declarations = buildFrameworkDeclarations(Arch::AArch64);
    return &Declarations;
  }
  if (Architecture == Arch::X64) {
    static const auto Declarations = buildFrameworkDeclarations(Arch::X64);
    return &Declarations;
  }
  return nullptr;
}

bool hasEmbeddedSDWebImageManagerDelegate(const BinaryImage &Image) {
  if (Image.Arch != Arch::AArch64)
    return false;
  const ObjCClass *Manager = nullptr;
  for (const auto &Class : Image.ObjCClasses)
    if (Class.Name == "SDWebImageManager") {
      if (Manager)
        return false;
      Manager = &Class;
    }
  if (!Manager || !Manager->Address)
    return false;
  unsigned Properties = 0, Methods = 0;
  for (const auto &Property : Image.ObjCProperties)
    if (Property.Owner == ObjCProperty::OwnerKind::Class &&
        Property.OwnerAddress == Manager->Address &&
        Property.ClassName == Manager->Name && Property.Name == "delegate" &&
        Property.Getter == "delegate" && !Property.IsClassProperty &&
        Property.Status == "supported" &&
        Property.TypeEncoding == "@\"<SDWebImageManagerDelegate>\"")
      ++Properties;
  for (const auto &Method : Image.ObjCMethods)
    if (Method.ClassName == Manager->Name && !Method.IsClassMethod &&
        Method.Selector ==
            "shouldBlockFailedURLWithURL:error:options:context:" &&
        Method.TypeEncoding == "B48@0:8@16@24Q32@40" && Method.TypeHint)
      ++Methods;
  return Properties == 1 && Methods == 1;
}

bool hasEmbeddedSDWebImageOptionsResult(const BinaryImage &Image) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64)
    return false;
  const ObjCClass *Manager = nullptr;
  const ObjCClass *Result = nullptr;
  for (const auto &Class : Image.ObjCClasses) {
    const ObjCClass **Match = Class.Name == "SDWebImageManager" ? &Manager
                              : Class.Name == "SDWebImageOptionsResult"
                                  ? &Result
                                  : nullptr;
    if (!Match)
      continue;
    if (*Match || !Class.Address)
      return false;
    *Match = &Class;
  }
  if (!Manager || !Result)
    return false;
  unsigned Factory = 0, Initializer = 0, Getter = 0, Property = 0;
  for (const auto &Method : Image.ObjCMethods) {
    if (Method.ClassName == Manager->Name &&
        Method.Selector == "processedResultForURL:options:context:") {
      if (Method.IsClassMethod || !Method.TypeHint ||
          Method.TypeEncoding != "@40@0:8@16Q24@32")
        return false;
      ++Factory;
    }
    if (Method.ClassName == Result->Name &&
        Method.Selector == "initWithOptions:context:") {
      if (Method.IsClassMethod || !Method.TypeHint ||
          Method.TypeEncoding != "@32@0:8Q16@24")
        return false;
      ++Initializer;
    }
    if (Method.ClassName == Result->Name && Method.Selector == "options") {
      if (Method.IsClassMethod || !Method.TypeHint ||
          Method.TypeEncoding != "Q16@0:8")
        return false;
      ++Getter;
    }
  }
  for (const auto &Candidate : Image.ObjCProperties)
    if (Candidate.Owner == ObjCProperty::OwnerKind::Class &&
        Candidate.OwnerAddress == Result->Address &&
        Candidate.ClassName == Result->Name && Candidate.Name == "options") {
      if (Candidate.Getter != "options" || Candidate.IsClassProperty ||
          Candidate.Status != "supported" || Candidate.TypeEncoding != "Q")
        return false;
      ++Property;
    }
  return Factory == 1 && Initializer == 1 && Getter == 1 && Property == 1;
}

bool hasEmbeddedDDLogFileInfoElement(const BinaryImage &Image,
                                     const ObjCReceiverTypeHint &Receiver) {
  // DDLogFileManager.sortedLogFileInfos is declared as
  // NSArray<DDLogFileInfo *> *. Runtime metadata retains NSArray, but erases
  // the element class. Only the exact getter result in this method may carry
  // that element class through NSArray's indexed-subscript operation.
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 ||
      Receiver.Origin != ObjCReceiverTypeHint::OriginKind::MethodEntry ||
      Receiver.ClassName != "DDLogFileManagerDefault" ||
      Receiver.IsClassMethod || Receiver.Steps.empty() ||
      Receiver.Steps.size() > 2 ||
      Receiver.Steps.front().TheKind !=
          ObjCReceiverTypeHint::TypeStep::Kind::MessageResult ||
      Receiver.Steps.front().Selector != "sortedLogFileInfos")
    return false;
  const ObjCClass *Manager = nullptr;
  const ObjCClass *Info = nullptr;
  for (const auto &Class : Image.ObjCClasses) {
    const ObjCClass **Match = Class.Name == "DDLogFileManagerDefault" ? &Manager
                              : Class.Name == "DDLogFileInfo"         ? &Info
                                                                      : nullptr;
    if (!Match)
      continue;
    if (*Match || !Class.Address)
      return false;
    *Match = &Class;
  }
  if (!Manager || !Info)
    return false;
  unsigned Entry = 0, Getter = 0, FileSize = 0, Property = 0;
  for (const auto &Method : Image.ObjCMethods) {
    const bool IsEntry = Method.ClassName == Manager->Name &&
                         Method.Selector == "deleteOldLogFilesWithError:";
    const bool IsGetter = Method.ClassName == Manager->Name &&
                          Method.Selector == "sortedLogFileInfos";
    const bool IsFileSize =
        Method.ClassName == Info->Name && Method.Selector == "fileSize";
    if (!IsEntry && !IsGetter && !IsFileSize)
      continue;
    if (Method.ClassAddress != (IsFileSize ? Info : Manager)->Address ||
        Method.CategoryAddress || !Method.CategoryName.empty() ||
        !Method.MetadataAddress || Method.IsClassMethod ||
        !objcMethodHasSourceBody(Method) ||
        !Image.isCodeAddress(Method.Implementation))
      return false;
    if (IsEntry) {
      if (Method.Implementation != Receiver.Address ||
          Method.TypeEncoding != "B24@0:8^@16")
        return false;
      ++Entry;
    } else if (IsGetter) {
      if (Method.TypeEncoding != "@16@0:8")
        return false;
      ++Getter;
    } else {
      if (Method.TypeEncoding != "Q16@0:8")
        return false;
      ++FileSize;
    }
  }
  for (const auto &Candidate : Image.ObjCProperties)
    if (Candidate.Owner == ObjCProperty::OwnerKind::Class &&
        Candidate.ClassName == Manager->Name &&
        Candidate.Name == "sortedLogFileInfos") {
      if (Candidate.OwnerAddress != Manager->Address ||
          Candidate.Getter != "sortedLogFileInfos" ||
          Candidate.IsClassProperty || !Candidate.MetadataAddress ||
          Candidate.Status != "supported" ||
          Candidate.TypeEncoding != "@\"NSArray\"")
        return false;
      ++Property;
    }
  return Entry == 1 && Getter == 1 && FileSize == 1 && Property == 1;
}

bool hasEmbeddedWMFCalendarMethod(const BinaryImage &Image,
                                  llvm::StringRef Selector,
                                  llvm::StringRef Encoding,
                                  bool IsClassMethod) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64)
    return false;
  const auto Calendar =
      objc::sdkReceiverDeclarations(Image, "NSCalendar", false, false, {});
  if (!Calendar.Present || !Calendar.Complete)
    return false;
  if (Selector == "wmf_components:fromDate:toDate:") {
    const auto Components = objc::sdkReceiverDeclarations(
        Image, "NSDateComponents", false, false, "year");
    if (!Components.Present || !Components.Complete ||
        Components.Members.size() != 1 || !Components.Members.front().Signature)
      return false;
  }
  unsigned Matches = 0;
  for (const auto &Method : Image.ObjCMethods) {
    if (Method.Selector != Selector)
      continue;
    if (Method.ClassName != "NSCalendar" ||
        Method.CategoryName != "WMFCommonCalendars" ||
        !Method.CategoryAddress || !Method.MetadataAddress ||
        !Method.Implementation || Method.IsClassMethod != IsClassMethod ||
        Method.TypeEncoding != Encoding || !objcMethodHasSourceBody(Method))
      return false;
    ++Matches;
  }
  return Matches == 1;
}

bool usesFramework(const BinaryImage &Image,
                   const FrameworkDeclarations &Framework) {
  llvm::StringRef Modules(Framework.Modules);
  while (!Modules.empty()) {
    const auto [Module, Rest] = Modules.split('|');
    for (const auto &Needed : Image.DynInfo.NeededLibs)
      if (Needed == Module)
        return true;
    Modules = Rest;
  }
  return false;
}
} // namespace

static std::optional<SelectorSignatures>
selectorSourceTypeHints(const BinaryImage &Image, llvm::StringRef Selector,
                        const SourceFunctionTypeHint *FormatSignature) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return std::nullopt;
  SelectorSignatures Result;
  auto Add = [&](SourceFunctionTypeHint Hint, bool SDK = false) {
    for (auto &Candidate : Result) {
      auto Merged = Candidate;
      if (!mergeSignature(Merged, Hint))
        continue;
      Candidate = std::move(Merged);
      if (SDK)
        Candidate.Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
      return;
    }
    if (SDK)
      Hint.Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
    Result.push_back(std::move(Hint));
  };
  // The dynamic receiver class is unknown. No declaration is selected by
  // visitation order or by assuming that the receiver has a framework class.
  auto Include = [&](const auto &Method) {
    if (Method.Selector != Selector)
      return true;
    if (!Method.TypeHint)
      return false;
    auto Hint = *Method.TypeHint;
    std::string Diagnostic;
    if (!assignDarwinObjCSourceABI(Hint, Image.Arch, Diagnostic))
      return false;
    Add(std::move(Hint));
    return true;
  };
  for (const auto &Method : Image.ObjCMethods)
    if (!Include(Method))
      return std::nullopt;
  for (const auto &Protocol : Image.ObjCProtocols)
    for (const auto &Method : Protocol.Methods)
      if (!Include(Method))
        return std::nullopt;
  for (const auto &Property : Image.ObjCProperties) {
    for (const auto &[Name, Hint] :
         {std::pair{&Property.Getter, &Property.GetterTypeHint},
          std::pair{&Property.Setter, &Property.SetterTypeHint}}) {
      if (Name->empty() || *Name != Selector)
        continue;
      if (!*Hint)
        return std::nullopt;
      Add(**Hint);
    }
  }
  if (const auto *Catalog = frameworkDeclarations(Image.Arch))
    for (const auto &[Name, Framework] : *Catalog) {
      if (!usesFramework(Image, Framework))
        continue;
      auto Found = Framework.Selectors.find(Selector.str());
      if (Found == Framework.Selectors.end())
        continue;
      // The format catalog currently belongs to Foundation. A negative
      // declaration from another framework must not inherit that contract.
      if (!Found->second) {
        if (Name == "Foundation" && FormatSignature) {
          Add(*FormatSignature, true);
          continue;
        }
        return std::nullopt;
      }
      for (const auto &Declared : *Found->second)
        Add(Declared, true);
    }
  // SDWebImage's optional delegate protocol is not necessarily emitted into
  // the consuming image. Its public declaration can still be authenticated by
  // the image's own manager class, typed delegate property, and matching
  // manager method. Do not apply this vendor declaration to another image that
  // merely uses the same selector spelling.
  if (Selector == "imageManager:shouldBlockFailedURL:withError:" &&
      hasEmbeddedSDWebImageManagerDelegate(Image)) {
    auto Hint = parseObjCMethodEncoding(Selector, "B40@0:8@16@24@32");
    std::string Diagnostic;
    if (!Hint || !assignDarwinObjCSourceABI(*Hint, Image.Arch, Diagnostic))
      return std::nullopt;
    Add(std::move(*Hint), true);
  }
  return Result;
}

static std::optional<SourceFunctionTypeHint>
selectorSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector,
                       const SourceFunctionTypeHint *FormatSignature) {
  auto Candidates = selectorSourceTypeHints(Image, Selector, FormatSignature);
  if (!Candidates || Candidates->size() != 1)
    return std::nullopt;
  return std::move(Candidates->front());
}

std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector) {
  return selectorSourceTypeHint(Image, Selector, nullptr);
}

std::optional<SourceFunctionTypeHint> objcSelectorSourceTypeHintForResultUse(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceABIValueLocation &RequiredResult,
    std::optional<NdTypeKind> RequiredType) {
  if ((RequiredResult.Kind != SourceABICarrierKind::IntegerRegister &&
       RequiredResult.Kind != SourceABICarrierKind::FloatingRegister) ||
      !RequiredResult.ValueBytes ||
      (RequiredType && *RequiredType != NdTypeKind::Int &&
       *RequiredType != NdTypeKind::Ptr && *RequiredType != NdTypeKind::Float))
    return std::nullopt;
  auto Candidates = selectorSourceTypeHints(Image, Selector, nullptr);
  if (!Candidates)
    return std::nullopt;
  std::optional<SourceFunctionTypeHint> Result;
  for (auto &Candidate : *Candidates) {
    if (RequiredType &&
        (!Candidate.ReturnType || Candidate.ReturnType->Kind != *RequiredType))
      continue;
    const auto &Location = Candidate.ReturnLocation;
    const uint16_t DefinedBytes =
        Location.ExtendTo32Bits ? std::max<uint16_t>(Location.ValueBytes, 4)
                                : Location.ValueBytes;
    const bool ExactFloating =
        RequiredResult.Kind == SourceABICarrierKind::FloatingRegister;
    if (Location.Kind != RequiredResult.Kind ||
        Location.RegisterOffset > RequiredResult.RegisterOffset ||
        RequiredResult.RegisterOffset - Location.RegisterOffset >
            DefinedBytes ||
        RequiredResult.ValueBytes >
            DefinedBytes -
                (RequiredResult.RegisterOffset - Location.RegisterOffset) ||
        (ExactFloating &&
         (Location.RegisterOffset != RequiredResult.RegisterOffset ||
          Location.ValueBytes != RequiredResult.ValueBytes)))
      continue;
    if (Result)
      return std::nullopt;
    Result = std::move(Candidate);
  }
  return Result;
}

static bool sameLocation(const SourceABIValueLocation &A,
                         const SourceABIValueLocation &B) {
  return A.Kind == B.Kind && A.RegisterOffset == B.RegisterOffset &&
         A.EntryStackOffset == B.EntryStackOffset &&
         A.ValueBytes == B.ValueBytes && A.ExtendTo32Bits == B.ExtendTo32Bits;
}

std::optional<SourceFunctionTypeHint>
objcMethodSourceTypeHint(const BinaryImage &Image, va_t Entry) {
  std::optional<SourceFunctionTypeHint> Result;
  for (const auto &Method : Image.ObjCMethods) {
    if (Method.Implementation != Entry)
      continue;
    if (!Method.TypeHint)
      return std::nullopt;
    auto Hint = *Method.TypeHint;
    std::string Diagnostic;
    if (!assignDarwinObjCSourceABI(Hint, Image.Arch, Diagnostic))
      return std::nullopt;
    if (Result && !equalSourceABIs(*Result, Hint))
      return std::nullopt;
    Result = std::move(Hint);
  }
  return Result;
}

static bool
hasEmbeddedSDImageLoaderNSErrorParameter(const BinaryImage &Image, va_t Entry,
                                         const SourceABIValueLocation &Source) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64)
    return false;
  const auto Caller = objcMethodSourceTypeHint(Image, Entry);
  if (!Caller || Caller->Parameters.size() != 6 ||
      !sameLocation(Caller->Parameters[3].Location, Source) ||
      !isObjCSelectorArgumentEvidenceType(Caller->Parameters[3].Type, true))
    return false;
  const ObjCClass *Downloader = nullptr;
  for (const auto &Class : Image.ObjCClasses)
    if (Class.Name == "SDWebImageDownloader") {
      if (Downloader || !Class.Address)
        return false;
      Downloader = &Class;
    }
  if (!Downloader)
    return false;
  constexpr llvm::StringLiteral Selector =
      "shouldBlockFailedURLWithURL:error:options:context:";
  constexpr llvm::StringLiteral Encoding = "B48@0:8@16@24Q32@40";
  unsigned Implementations = 0;
  for (const auto &Method : Image.ObjCMethods)
    if (Method.Implementation == Entry) {
      if (Method.ClassAddress != Downloader->Address ||
          Method.ClassName != Downloader->Name || Method.IsClassMethod ||
          Method.Selector != Selector || Method.TypeEncoding != Encoding ||
          !Method.TypeHint)
        return false;
      ++Implementations;
    }
  if (Implementations != 1)
    return false;
  unsigned Protocols = 0, Declarations = 0;
  for (const auto &Protocol : Image.ObjCProtocols)
    if (Protocol.Name == "SDImageLoader") {
      if (!Protocol.Address || Protocol.Status != "recovered")
        return false;
      ++Protocols;
      for (const auto &Method : Protocol.Methods)
        if (Method.Selector == Selector) {
          if (Method.IsClassMethod || !Method.IsOptional ||
              Method.TypeEncoding != Encoding || !Method.TypeHint)
            return false;
          ++Declarations;
        }
    }
  return Protocols == 1 && Declarations == 1;
}

std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHintForArgumentTypeUse(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceCallTypeHint::SelectorArgumentTypeEvidence &Evidence) {
  if (!Evidence.MethodEntry)
    return std::nullopt;
  if (Evidence.Parameter == 0) {
    // The SDImageLoader declaration names this method's error parameter as
    // NSError*. Runtime encodings erase that class. Revalidate the embedded
    // vendor method and protocol before selecting Foundation's NSError.code
    // declaration from the otherwise conflicting selector-wide signatures.
    if (Selector != "code" || !Evidence.ConsumedAsObject ||
        !hasEmbeddedSDImageLoaderNSErrorParameter(Image, Evidence.MethodEntry,
                                                  Evidence.Source))
      return std::nullopt;
    const auto NSError =
        objc::sdkReceiverDeclarations(Image, "NSError", false, false, Selector);
    if (!NSError.Present || !NSError.Complete || NSError.Members.size() != 1 ||
        !NSError.Members.front().Signature)
      return std::nullopt;
    const auto Candidates = selectorSourceTypeHints(Image, Selector, nullptr);
    if (!Candidates)
      return std::nullopt;
    std::optional<SourceFunctionTypeHint> Result;
    for (const auto &Candidate : *Candidates)
      if (Candidate.Origin == SourceFunctionTypeHint::OriginKind::ObjCSDK &&
          equalSourceABIs(Candidate, *NSError.Members.front().Signature)) {
        if (Result)
          return std::nullopt;
        Result = Candidate;
      }
    return Result;
  }
  if (Evidence.Parameter < 2)
    return std::nullopt;
  const auto Caller = objcMethodSourceTypeHint(Image, Evidence.MethodEntry);
  if (!Caller)
    return std::nullopt;
  const SourceParameterTypeHint *Source = nullptr;
  for (const auto &Parameter : Caller->Parameters)
    if (sameLocation(Parameter.Location, Evidence.Source)) {
      if (Source)
        return std::nullopt;
      Source = &Parameter;
    }
  if (!Source || !isObjCSelectorArgumentEvidenceType(Source->Type,
                                                     Evidence.ConsumedAsObject))
    return std::nullopt;
  auto Candidates = selectorSourceTypeHints(Image, Selector, nullptr);
  if (!Candidates)
    return std::nullopt;
  std::optional<SourceFunctionTypeHint> Result;
  for (auto &Candidate : *Candidates) {
    if (Evidence.Parameter >= Candidate.Parameters.size() ||
        !equalSourceTypes(Candidate.Parameters[Evidence.Parameter].Type,
                          Source->Type))
      continue;
    if (Result)
      return std::nullopt;
    Result = std::move(Candidate);
  }
  return Result;
}

std::optional<SourceFunctionTypeHint> objcMethodForwardingSourceTypeHint(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceCallTypeHint::SelectorForwardingEvidence &Evidence) {
  if (!Evidence.MethodEntry || Evidence.ReceiverSourceParameter < 2 ||
      Selector.count(':') != Evidence.ArgumentSourceParameters.size())
    return std::nullopt;
  const auto Caller = objcMethodSourceTypeHint(Image, Evidence.MethodEntry);
  if (!Caller || !Caller->ReturnType || Caller->Parameters.size() < 3 ||
      Evidence.ReceiverSourceParameter >= Caller->Parameters.size())
    return std::nullopt;
  const auto CompleteIntegerCarrier = [](const SourceParameterTypeHint &P) {
    return P.Type &&
           (P.Type->Kind == NdTypeKind::Int ||
            P.Type->Kind == NdTypeKind::Ptr) &&
           P.Type->Size == 8 &&
           P.Location.Kind == SourceABICarrierKind::IntegerRegister &&
           P.Location.ValueBytes == 8 && P.Components.empty();
  };
  const auto &Receiver = Caller->Parameters[Evidence.ReceiverSourceParameter];
  if (!CompleteIntegerCarrier(Receiver) ||
      Receiver.Type->Kind != NdTypeKind::Ptr || !Receiver.Type->Pointee ||
      Receiver.Type->Pointee->Kind != NdTypeKind::Void)
    return std::nullopt;

  SourceFunctionTypeHint Result;
  Result.Origin = SourceFunctionTypeHint::OriginKind::ObjCRuntime;
  Result.ReturnType = Caller->ReturnType;
  Result.Parameters.assign(Caller->Parameters.begin(),
                           Caller->Parameters.begin() + 2);
  for (const auto Parameter : Evidence.ArgumentSourceParameters) {
    if (Parameter < 2 || Parameter >= Caller->Parameters.size() ||
        !CompleteIntegerCarrier(Caller->Parameters[Parameter]))
      return std::nullopt;
    auto Forwarded = Caller->Parameters[Parameter];
    Forwarded.Name = "arg" + std::to_string(Result.Parameters.size() - 2);
    Forwarded.Location = {};
    Forwarded.Components.clear();
    Result.Parameters.push_back(std::move(Forwarded));
  }
  for (auto &Parameter : Result.Parameters) {
    Parameter.Location = {};
    Parameter.Components.clear();
  }
  std::string Diagnostic;
  if (!assignDarwinObjCSourceABI(Result, Image.Arch, Diagnostic) ||
      !sameLocation(Result.ReturnLocation, Caller->ReturnLocation))
    return std::nullopt;
  const bool SameReturnComponents =
      Result.ReturnComponents.size() == Caller->ReturnComponents.size() &&
      std::equal(Result.ReturnComponents.begin(), Result.ReturnComponents.end(),
                 Caller->ReturnComponents.begin(), sameLocation);
  if (!SameReturnComponents)
    return std::nullopt;

  return Result;
}

std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHintForForwardingUse(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceCallTypeHint::SelectorForwardingEvidence &Evidence) {
  auto Result = objcMethodForwardingSourceTypeHint(Image, Selector, Evidence);
  if (!Result)
    return std::nullopt;

  // Complete declarations remain authoritative. A forwarding wrapper may
  // fill an absent selector declaration, but it cannot override an incomplete
  // or differently typed declaration elsewhere in the active image/SDK.
  const auto Candidates = selectorSourceTypeHints(Image, Selector, nullptr);
  if (!Candidates)
    return std::nullopt;
  for (auto Candidate : *Candidates) {
    if (!equalSourceTypes(Candidate.ReturnType, Result->ReturnType) ||
        Candidate.Parameters.size() != Result->Parameters.size())
      return std::nullopt;
    for (size_t I = 0; I < Candidate.Parameters.size(); ++I) {
      if (!equalSourceTypes(Candidate.Parameters[I].Type,
                            Result->Parameters[I].Type))
        return std::nullopt;
      Candidate.Parameters[I].Name = Result->Parameters[I].Name;
    }
    Candidate.Origin = Result->Origin;
    if (!equalSourceABIs(Candidate, *Result))
      return std::nullopt;
  }
  return Result;
}

bool isObjCSelectorArgumentEvidenceType(const TypeRef &Type,
                                        bool ConsumedAsObject) {
  // The declaration, not the machine width alone, supplies this evidence.
  // Keep it to complete Darwin pointer carriers and require a unique exact
  // source-type match below. An opaque object pointer additionally needs an
  // authenticated object consumer; pointer-to-pointer evidence retains its
  // original declaration-only contract.
  if (!Type || Type->Kind != NdTypeKind::Ptr || Type->Size != 8 ||
      !Type->Pointee)
    return false;
  return (Type->Pointee->Kind == NdTypeKind::Ptr && Type->Pointee->Size == 8) ||
         (ConsumedAsObject && Type->Pointee->Kind == NdTypeKind::Void);
}

std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHintForArgumentStorageUse(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceCallTypeHint::SelectorArgumentStorageEvidence &Evidence) {
  if (Evidence.Parameter < 2 || Evidence.FrameOffset >= 0)
    return std::nullopt;
  auto Candidates = selectorSourceTypeHints(Image, Selector, nullptr);
  if (!Candidates)
    return std::nullopt;
  std::optional<SourceFunctionTypeHint> Result;
  for (auto &Candidate : *Candidates) {
    if (Evidence.Parameter >= Candidate.Parameters.size())
      continue;
    const auto &Type = Candidate.Parameters[Evidence.Parameter].Type;
    if (!Type || Type->Kind != NdTypeKind::Ptr || Type->Size != 8 ||
        !Type->Pointee || Type->Pointee->Kind != NdTypeKind::Ptr ||
        Type->Pointee->Size != 8)
      continue;
    if (Result)
      return std::nullopt;
    Result = std::move(Candidate);
  }
  return Result;
}

std::optional<std::string>
objcSelectorOutParameterClass(const BinaryImage &Image,
                              llvm::StringRef Selector, unsigned Parameter) {
  const auto Signature = objcSelectorSourceTypeHint(Image, Selector);
  if (!Signature || Parameter < 2 || Parameter >= Signature->Parameters.size())
    return std::nullopt;
  const auto &Type = Signature->Parameters[Parameter].Type;
  if (!Type || Type->Kind != NdTypeKind::Ptr || Type->Size != 8 ||
      !Type->Pointee || Type->Pointee->Kind != NdTypeKind::Ptr ||
      Type->Pointee->Size != 8)
    return std::nullopt;
  // Runtime metadata preserves only @ inside ^@. A local method or protocol
  // declaration can therefore agree on the physical ABI while naming a
  // different pointee class in source; do not inherit an SDK class across it.
  for (const auto &Method : Image.ObjCMethods)
    if (Method.Selector == Selector)
      return std::nullopt;
  for (const auto &Protocol : Image.ObjCProtocols)
    for (const auto &Method : Protocol.Methods)
      if (Method.Selector == Selector)
        return std::nullopt;
  for (const auto &Property : Image.ObjCProperties)
    if (Property.Getter == Selector || Property.Setter == Selector)
      return std::nullopt;
  return objc::sdkSelectorOutParameterClass(Image, Selector, Parameter);
}

std::optional<ObjCReceiverTypeHint>
objcMethodReceiverTypeHint(const BinaryImage &Image, va_t Entry) {
  if (!Entry || Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return std::nullopt;
  std::optional<ObjCReceiverTypeHint> Result;
  for (const auto &Method : Image.ObjCMethods) {
    if (Method.Implementation != Entry)
      continue;
    if (Method.ClassName.empty() || !Method.TypeHint ||
        (Result && (Result->ClassName != Method.ClassName ||
                    Result->IsClassMethod != Method.IsClassMethod)))
      return std::nullopt;
    auto Signature = *Method.TypeHint;
    std::string Diagnostic;
    if (!assignDarwinObjCSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    Result =
        ObjCReceiverTypeHint{ObjCReceiverTypeHint::OriginKind::MethodEntry,
                             Entry, Method.ClassName, Method.IsClassMethod};
  }
  return Result;
}

std::optional<ObjCReceiverTypeHint>
objcMethodParameterReceiverTypeHint(const BinaryImage &Image, va_t Entry,
                                    unsigned Parameter) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 || Image.Arch != Arch::AArch64 || !Entry)
    return std::nullopt;
  // These WMF source declarations name an NSManagedObjectContext argument,
  // although the Objective-C method encoding retains only its object carrier.
  // Keep the exact owner, selector, encoding and parameter position together;
  // performBlock: still needs its separate Core Data lifetime contract.
  struct ManagedContextDeclaration {
    const char *Owner;
    const char *Selector;
    const char *Encoding;
    unsigned Parameter;
  };
  static constexpr ManagedContextDeclaration ManagedContexts[] = {
      {"WMFSuggestedEditsContentSource",
       "loadNewContentInManagedObjectContext:force:completion:",
       "v36@0:8@16B24@?28", 2},
      {"WMFContinueReadingContentSource",
       "loadNewContentInManagedObjectContext:force:completion:",
       "v36@0:8@16B24@?28", 2},
      {"WMFNearbyContentSource",
       "loadNewContentInManagedObjectContext:force:completion:",
       "v36@0:8@16B24@?28", 2},
      {"WMFFeedContentSource",
       "saveContentForFeedDay:pageViews:onDate:inManagedObjectContext:"
       "completion:",
       "v56@0:8@16@24@32@40@?48", 5},
      {"WMFAnnouncementsContentSource",
       "saveAnnouncements:inManagedObjectContext:completion:",
       "v40@0:8@16@24@?32", 3},
      {"WMFOnThisDayContentSource",
       "loadContentForDate:inManagedObjectContext:force:completion:",
       "v44@0:8@16@24B32@?36", 3},
      {"WMFRandomContentSource",
       "loadContentForDate:inManagedObjectContext:force:completion:",
       "v44@0:8@16@24B32@?36", 3},
      {"WMFNearbyContentSource",
       "getGroupForLocation:inManagedObjectContext:force:completion:failure:",
       "v52@0:8@16@24B32@?36@?44", 3},
      {"WMFRelatedPagesContentSource",
       "loadContentForDate:inManagedObjectContext:force:addNewContent:"
       "completion:",
       "v48@0:8@16@24B32B36@?40", 3},
  };
  const ManagedContextDeclaration *Context = nullptr;
  for (const auto &Method : Image.ObjCMethods) {
    if (Method.Implementation != Entry)
      continue;
    for (const auto &Candidate : ManagedContexts)
      if (Candidate.Parameter == Parameter &&
          Method.ClassName == Candidate.Owner &&
          Method.Selector == Candidate.Selector) {
        if (Context)
          return std::nullopt;
        Context = &Candidate;
      }
  }
  if (Context) {
    const auto SDK = objc::sdkReceiverDeclarations(
        Image, "NSManagedObjectContext", false, false, "performBlock:");
    if (!SDK.Present || !SDK.Complete || SDK.Members.size() != 1 ||
        !SDK.Members.front().Signature)
      return std::nullopt;
    const ObjCClass *Owner = nullptr;
    for (const auto &Class : Image.ObjCClasses)
      if (Class.Name == Context->Owner) {
        if (Owner)
          return std::nullopt;
        Owner = &Class;
      }
    if (!Owner || !Owner->Address)
      return std::nullopt;
    const ObjCMethod *Found = nullptr;
    for (const auto &Method : Image.ObjCMethods) {
      if (Method.Implementation != Entry &&
          !(Method.ClassName == Context->Owner &&
            Method.Selector == Context->Selector))
        continue;
      if (Found || Method.Implementation != Entry ||
          Method.ClassAddress != Owner->Address || Method.CategoryAddress ||
          !Method.CategoryName.empty() || !Method.MetadataAddress ||
          Method.IsClassMethod || Method.TypeEncoding != Context->Encoding ||
          !objcMethodHasSourceBody(Method) || !Image.isCodeAddress(Entry))
        return std::nullopt;
      Found = &Method;
    }
    if (!Found)
      return std::nullopt;
    const auto Signature = objcMethodSourceTypeHint(Image, Entry);
    if (!Signature ||
        Signature->Parameters.size() !=
            2 + llvm::StringRef(Context->Selector).count(':') ||
        Parameter >= Signature->Parameters.size() ||
        !Signature->Parameters[Parameter].Type ||
        Signature->Parameters[Parameter].Type->Kind != NdTypeKind::Ptr ||
        Signature->Parameters[Parameter].Type->Size != 8 ||
        Signature->Parameters[Parameter].Location.Kind !=
            SourceABICarrierKind::IntegerRegister ||
        Signature->Parameters[Parameter].Location.RegisterOffset !=
            getTargetRegInfo(Arch::AArch64).IntParamRegs[Parameter] ||
        Signature->Parameters[Parameter].Location.ValueBytes != 8)
      return std::nullopt;
    ObjCReceiverTypeHint Result;
    Result.Origin = ObjCReceiverTypeHint::OriginKind::MethodParameter;
    Result.Address = Entry;
    Result.ClassName = "NSManagedObjectContext";
    Result.SourceParameter = Parameter;
    return Result;
  }
  if (Parameter != 2)
    return std::nullopt;
  // Objective-C's runtime encoding erases these source parameter classes.
  // WMFFeedContentSource.m declares news as NSArray<WMFFeedNewsStory *> *;
  // its top-read argument is WMFFeedTopReadResponse *.
  // WMFAnnouncementsContentSource.m declares announcements as NSArray *.
  // CocoaLumberjack 3.6.2 DDFileLogger.m declares mostRecentLogFileInfo as
  // DDLogFileInfo *. Recheck the exact embedded method and its ABI below.
  constexpr llvm::StringLiteral NewsSelector =
      "saveGroupForNews:pageViews:date:inManagedObjectContext:";
  constexpr llvm::StringLiteral TopReadSelector =
      "saveGroupForTopRead:pageViews:date:inManagedObjectContext:";
  constexpr llvm::StringLiteral AnnouncementsSelector =
      "saveAnnouncements:inManagedObjectContext:completion:";
  constexpr llvm::StringLiteral LogSelector = "lt_shouldLogFileBeArchived:";
  const auto Matches = [&](llvm::StringRef ClassName,
                           llvm::StringRef Selector) {
    return std::any_of(Image.ObjCMethods.begin(), Image.ObjCMethods.end(),
                       [&](const ObjCMethod &Method) {
                         return Method.Implementation == Entry &&
                                Method.ClassName == ClassName &&
                                Method.Selector == Selector;
                       });
  };
  const bool News = Matches("WMFFeedContentSource", NewsSelector);
  const bool TopRead = Matches("WMFFeedContentSource", TopReadSelector);
  const bool Announcements =
      Matches("WMFAnnouncementsContentSource", AnnouncementsSelector);
  const bool LogFile = Matches("DDFileLogger", LogSelector);
  if (unsigned(News) + unsigned(TopRead) + unsigned(Announcements) +
          unsigned(LogFile) !=
      1)
    return std::nullopt;
  const llvm::StringRef OwnerName = LogFile ? "DDFileLogger"
                                    : Announcements
                                        ? "WMFAnnouncementsContentSource"
                                        : "WMFFeedContentSource";
  const llvm::StringRef Selector = LogFile         ? LogSelector
                                   : Announcements ? AnnouncementsSelector
                                   : TopRead       ? TopReadSelector
                                                   : NewsSelector;
  const llvm::StringRef Encoding = LogFile         ? "B24@0:8@16"
                                   : Announcements ? "v40@0:8@16@24@?32"
                                                   : "v48@0:8@16@24@32@40";
  const llvm::StringRef ParameterClass = LogFile   ? "DDLogFileInfo"
                                         : TopRead ? "WMFFeedTopReadResponse"
                                                   : "NSArray";
  if (News || Announcements) {
    const auto Array = objc::sdkReceiverDeclarations(
        Image, "NSArray", false, false, "enumerateObjectsUsingBlock:");
    if (!Array.Present || !Array.Complete)
      return std::nullopt;
  } else if (TopRead) {
    const ObjCClass *Response = nullptr;
    for (const auto &Class : Image.ObjCClasses)
      if (Class.Name == ParameterClass) {
        if (Response)
          return std::nullopt;
        Response = &Class;
      }
    if (!Response || !Response->Address)
      return std::nullopt;
    const ObjCProperty *Previews = nullptr;
    for (const auto &Property : Image.ObjCProperties)
      if (Property.ClassName == ParameterClass &&
          Property.Name == "articlePreviews") {
        if (Previews || Property.Owner != ObjCProperty::OwnerKind::Class ||
            Property.OwnerAddress != Response->Address ||
            !Property.MetadataAddress || Property.Getter != "articlePreviews" ||
            Property.IsClassProperty || Property.Status != "supported" ||
            Property.TypeEncoding != "@\"NSArray\"")
          return std::nullopt;
        Previews = &Property;
      }
    if (!Previews)
      return std::nullopt;
  } else {
    const ObjCClass *FileInfo = nullptr;
    for (const auto &Class : Image.ObjCClasses)
      if (Class.Name == ParameterClass) {
        if (FileInfo)
          return std::nullopt;
        FileInfo = &Class;
      }
    if (!FileInfo || !FileInfo->Address)
      return std::nullopt;
    const ObjCMethod *FileSize = nullptr;
    for (const auto &Method : Image.ObjCMethods)
      if (Method.ClassName == ParameterClass && Method.Selector == "fileSize") {
        if (FileSize || Method.ClassAddress != FileInfo->Address ||
            Method.CategoryAddress || !Method.CategoryName.empty() ||
            !Method.MetadataAddress || Method.IsClassMethod ||
            Method.TypeEncoding != "Q16@0:8" ||
            !objcMethodHasSourceBody(Method) ||
            !Image.isCodeAddress(Method.Implementation))
          return std::nullopt;
        FileSize = &Method;
      }
    if (!FileSize)
      return std::nullopt;
  }
  const ObjCClass *Owner = nullptr;
  for (const auto &Class : Image.ObjCClasses)
    if (Class.Name == OwnerName) {
      if (Owner)
        return std::nullopt;
      Owner = &Class;
    }
  if (!Owner || !Owner->Address)
    return std::nullopt;
  const ObjCMethod *Found = nullptr;
  for (const auto &Method : Image.ObjCMethods) {
    if (Method.Implementation != Entry &&
        !(Method.ClassName == Owner->Name && Method.Selector == Selector))
      continue;
    if (Found || Method.Implementation != Entry ||
        Method.ClassAddress != Owner->Address || Method.CategoryAddress ||
        !Method.CategoryName.empty() || !Method.MetadataAddress ||
        Method.IsClassMethod || Method.Selector != Selector ||
        Method.TypeEncoding != Encoding || !objcMethodHasSourceBody(Method) ||
        !Image.isCodeAddress(Entry))
      return std::nullopt;
    Found = &Method;
  }
  if (!Found)
    return std::nullopt;
  const auto Signature = objcMethodSourceTypeHint(Image, Entry);
  if (!Signature ||
      Signature->Parameters.size() != (LogFile         ? 3U
                                       : Announcements ? 5U
                                                       : 6U) ||
      !Signature->Parameters[Parameter].Type ||
      Signature->Parameters[Parameter].Type->Kind != NdTypeKind::Ptr ||
      Signature->Parameters[Parameter].Type->Size != 8 ||
      Signature->Parameters[Parameter].Location.Kind !=
          SourceABICarrierKind::IntegerRegister ||
      Signature->Parameters[Parameter].Location.RegisterOffset !=
          getTargetRegInfo(Arch::AArch64).IntParamRegs[Parameter] ||
      Signature->Parameters[Parameter].Location.ValueBytes != 8)
    return std::nullopt;
  ObjCReceiverTypeHint Result;
  Result.Origin = ObjCReceiverTypeHint::OriginKind::MethodParameter;
  Result.Address = Entry;
  Result.ClassName = ParameterClass.str();
  Result.SourceParameter = Parameter;
  return Result;
}

std::optional<ObjCReceiverTypeHint>
objcBlockParameterReceiverTypeHint(const BinaryImage &Image, va_t Invoke,
                                   va_t Descriptor, uint32_t Flags,
                                   unsigned Parameter) {
  if (!Invoke || !Image.isCodeAddress(Invoke) || !Descriptor || !Parameter)
    return std::nullopt;
  std::string Diagnostic;
  const auto Block =
      readObjCBlockDescriptor(Image, Descriptor, Flags, Diagnostic);
  if (!Block || !Block->InvokeTypeHint || !Block->Limitations.empty() ||
      Parameter >= Block->InvokeTypeHint->Parameters.size())
    return std::nullopt;
  const auto Class = objcBlockObjectParameterClass(Block->Signature, Parameter);
  const auto &Argument = Block->InvokeTypeHint->Parameters[Parameter];
  const auto &Location = Argument.Location;
  if (!Class || !Argument.Type || Argument.Type->Kind != NdTypeKind::Ptr ||
      Argument.Type->Size != 8 ||
      Location.Kind != SourceABICarrierKind::IntegerRegister ||
      Location.ValueBytes != 8 || Parameter >= 8 ||
      Location.RegisterOffset !=
          getTargetRegInfo(Image.Arch).IntParamRegs[Parameter])
    return std::nullopt;
  ObjCReceiverTypeHint Result;
  Result.Origin = ObjCReceiverTypeHint::OriginKind::BlockParameter;
  Result.Address = Invoke;
  Result.ClassName = *Class;
  Result.SourceParameter = Parameter;
  Result.BlockDescriptorAddress = Descriptor;
  Result.BlockDescriptorFlags = Flags;
  return Result;
}

namespace {
bool validReceiverRoot(const BinaryImage &Image,
                       const ObjCReceiverTypeHint &Receiver) {
  if (!Receiver.Address || Receiver.ClassName.empty() ||
      (Receiver.Origin != ObjCReceiverTypeHint::OriginKind::MethodParameter &&
       Receiver.Origin != ObjCReceiverTypeHint::OriginKind::BlockParameter &&
       Receiver.SourceParameter) ||
      (Receiver.Origin != ObjCReceiverTypeHint::OriginKind::BlockParameter &&
       (Receiver.BlockDescriptorAddress || Receiver.BlockDescriptorFlags)) ||
      Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return false;
  switch (Receiver.Origin) {
  case ObjCReceiverTypeHint::OriginKind::MethodEntry: {
    if (!Receiver.OutParameters.empty())
      return false;
    const auto Expected = objcMethodReceiverTypeHint(Image, Receiver.Address);
    return Expected && Expected->ClassName == Receiver.ClassName &&
           Expected->IsClassMethod == Receiver.IsClassMethod;
  }
  case ObjCReceiverTypeHint::OriginKind::MethodParameter: {
    if (Receiver.IsClassMethod || !Receiver.OutParameters.empty())
      return false;
    const auto Expected = objcMethodParameterReceiverTypeHint(
        Image, Receiver.Address, Receiver.SourceParameter);
    return Expected && Expected->ClassName == Receiver.ClassName &&
           Expected->SourceParameter == Receiver.SourceParameter;
  }
  case ObjCReceiverTypeHint::OriginKind::BlockParameter: {
    if (Receiver.IsClassMethod || !Receiver.OutParameters.empty() ||
        !Receiver.BlockDescriptorAddress)
      return false;
    const auto Expected = objcBlockParameterReceiverTypeHint(
        Image, Receiver.Address, Receiver.BlockDescriptorAddress,
        Receiver.BlockDescriptorFlags, Receiver.SourceParameter);
    return Expected && Expected->ClassName == Receiver.ClassName;
  }
  case ObjCReceiverTypeHint::OriginKind::ClassReference: {
    if (!Receiver.OutParameters.empty())
      return false;
    const auto Found = Image.ObjCSourceReferences.find(Receiver.Address);
    return Receiver.IsClassMethod &&
           Found != Image.ObjCSourceReferences.end() &&
           Found->second.Address == Receiver.Address &&
           Found->second.TheKind == ObjCSourceReference::Kind::Class &&
           Found->second.Size == 8 && Found->second.Name == Receiver.ClassName;
  }
  case ObjCReceiverTypeHint::OriginKind::OutParameter: {
    if (Receiver.IsClassMethod || Receiver.OutParameters.empty() ||
        Receiver.OutParameters.size() > 8 ||
        Receiver.Address != Receiver.OutParameters.front().Address ||
        !std::is_sorted(Receiver.OutParameters.begin(),
                        Receiver.OutParameters.end()) ||
        std::adjacent_find(Receiver.OutParameters.begin(),
                           Receiver.OutParameters.end()) !=
            Receiver.OutParameters.end())
      return false;
    for (const auto &Root : Receiver.OutParameters) {
      if (!Root.Address || Root.Selector.empty() || Root.Parameter < 2)
        return false;
      const auto Found = Image.ObjCSourceReferences.find(Root.Address);
      const auto Class =
          objcSelectorOutParameterClass(Image, Root.Selector, Root.Parameter);
      if (Found == Image.ObjCSourceReferences.end() ||
          Found->second.Address != Root.Address ||
          Found->second.TheKind != ObjCSourceReference::Kind::Selector ||
          Found->second.Size != 8 || Found->second.Name != Root.Selector ||
          !Class || *Class != Receiver.ClassName)
        return false;
    }
    return true;
  }
  }
  return false;
}

const ObjCIvar *receiverIvar(const BinaryImage &Image, std::string ClassName,
                             uint64_t Key, bool BySlot) {
  const ObjCIvar *Result = nullptr;
  std::set<std::string> Visited;
  size_t Remaining = 4096;
  while (!ClassName.empty()) {
    if (Visited.size() >= 64 || !Visited.insert(ClassName).second)
      return nullptr;
    const ObjCClass *Class = nullptr;
    for (const auto &Candidate : Image.ObjCClasses)
      if (Candidate.Name == ClassName) {
        if (Class)
          return nullptr;
        Class = &Candidate;
      }
    if (!Class)
      break; // No external layout is invented beyond the recorded lineage.
    if ((Class->IvarStatus != "recovered" &&
         !(BySlot && Class->IvarStatus == "runtime")) ||
        (Class->RootClass ? Class->InheritanceStatus != "root" ||
                                !Class->SuperclassName.empty()
                          : Class->InheritanceStatus != "resolved" ||
                                Class->SuperclassName.empty()))
      return nullptr;
    for (const auto &Ivar : Class->Ivars) {
      if (!Remaining--)
        return nullptr;
      const bool Match =
          BySlot ? Ivar.OffsetAddress == Key
                 : Ivar.Offset && Key < uint64_t(*Ivar.Offset) + Ivar.Size &&
                       *Ivar.Offset < Key + 8;
      if (!Match)
        continue;
      if (Result || !Ivar.MetadataAddress || !Ivar.OffsetAddress ||
          Ivar.Size != 8 || (!BySlot && Ivar.Offset != Key) ||
          (!Ivar.Offset && Class->IvarStatus != "runtime") ||
          (Ivar.Offset &&
           (*Ivar.Offset < Class->InstanceStart ||
            !rangeInBounds(*Ivar.Offset, Ivar.Size, Class->InstanceSize))))
        return nullptr;
      const auto Ref = Image.ObjCSourceReferences.find(Ivar.OffsetAddress);
      if (Ref == Image.ObjCSourceReferences.end() ||
          Ref->second.Address != Ivar.OffsetAddress ||
          Ref->second.TheKind != ObjCSourceReference::Kind::IvarOffset ||
          (Ref->second.Size != 4 && Ref->second.Size != 8) ||
          Ref->second.Name != Ivar.Name ||
          Ref->second.ClassName != Class->Name ||
          (!objcEncodedObjectClass(Ivar.TypeEncoding) &&
           !objcEncodedObjectProtocol(Ivar.TypeEncoding)))
        return nullptr;
      Result = &Ivar;
    }
    ClassName = Class->SuperclassName;
  }
  return Result;
}

struct ReceiverType {
  std::string ClassName;
  bool IsClassMethod = false;
  bool IncludeSubclasses = false;
  bool IsProtocol = false;
};

enum class ReceiverResultMode { ObjCNilDispatch, ProvenNonNullSelf };

ObjCReceiverDeclaration receiverDeclaration(
    const BinaryImage &Image, llvm::StringRef Selector,
    const ReceiverType &Type,
    ReceiverResultMode ResultMode = ReceiverResultMode::ObjCNilDispatch);

bool receiverProtocolKnown(const BinaryImage &Image, llvm::StringRef Name) {
  bool Present = false;
  for (const auto &Protocol : Image.ObjCProtocols) {
    if (Protocol.Name != Name)
      continue;
    if (Present || Protocol.Status != "recovered")
      return false;
    Present = true;
  }
  const auto SDK = objc::sdkReceiverDeclarations(Image, Name, true, false, {});
  if (SDK.Present)
    return SDK.Complete;
  return Present || (Name == "SDWebImageManagerDelegate" &&
                     hasEmbeddedSDWebImageManagerDelegate(Image));
}

std::optional<ReceiverType> receiverType(const BinaryImage &Image,
                                         const ObjCReceiverTypeHint &Receiver) {
  if (Receiver.Steps.size() > 8 || !validReceiverRoot(Image, Receiver))
    return std::nullopt;
  ReceiverType Result{Receiver.ClassName, Receiver.IsClassMethod,
                      Receiver.Origin !=
                          ObjCReceiverTypeHint::OriginKind::ClassReference,
                      false};
  for (const auto &Access : Receiver.Steps) {
    if (Access.TheKind == ObjCReceiverTypeHint::TypeStep::Kind::MessageResult) {
      if (Access.Selector.empty() || Access.OffsetSlot || Access.ByteOffset ||
          Access.OffsetWidth)
        return std::nullopt;
      const auto Declaration =
          receiverDeclaration(Image, Access.Selector, Result);
      const bool DDLogFileInfoElement =
          Receiver.Steps.size() == 2 && &Access == &Receiver.Steps[1] &&
          Access.Selector == "objectAtIndexedSubscript:" &&
          Result.ClassName == "NSArray" && !Result.IsClassMethod &&
          !Result.IsProtocol &&
          hasEmbeddedDDLogFileInfoElement(Image, Receiver) &&
          Declaration.Signature && Declaration.Signature->ReturnType &&
          Declaration.Signature->ReturnType->Kind == NdTypeKind::Ptr &&
          Declaration.Signature->ReturnType->Size == 8 &&
          !Declaration.ReturnClass && !Declaration.ReturnProtocol;
      if (!Declaration.Signature || Declaration.RequiresGlobalAgreement ||
          (!Declaration.ReturnClass && !Declaration.ReturnProtocol &&
           !DDLogFileInfoElement) ||
          (Declaration.ReturnClass && Declaration.ReturnProtocol))
        return std::nullopt;
      if (Declaration.ReturnClass || DDLogFileInfoElement)
        Result = {DDLogFileInfoElement ? "DDLogFileInfo"
                                       : *Declaration.ReturnClass,
                  false, true, false};
      else if (receiverProtocolKnown(Image, *Declaration.ReturnProtocol))
        Result = {*Declaration.ReturnProtocol, false, false, true};
      else
        return std::nullopt;
      continue;
    }
    if (Access.TheKind != ObjCReceiverTypeHint::TypeStep::Kind::IvarLoad ||
        !Access.Selector.empty() || Result.IsClassMethod)
      return std::nullopt;
    const auto *Ivar =
        receiverIvar(Image, Result.ClassName, Access.OffsetSlot, true);
    if (!Ivar || (Access.ByteOffset && *Access.ByteOffset != Ivar->Offset) ||
        Image.ObjCSourceReferences.at(Access.OffsetSlot).Size !=
            Access.OffsetWidth)
      return std::nullopt;
    const auto ClassName = objcEncodedObjectClass(Ivar->TypeEncoding);
    const auto ProtocolName = objcEncodedObjectProtocol(Ivar->TypeEncoding);
    if ((!ClassName && !ProtocolName) || (ClassName && ProtocolName))
      return std::nullopt;
    if (ProtocolName && !receiverProtocolKnown(Image, *ProtocolName))
      return std::nullopt;
    Result.ClassName = ClassName ? *ClassName : *ProtocolName;
    Result.IncludeSubclasses = ClassName.has_value();
    Result.IsProtocol = ProtocolName.has_value();
  }
  return Result;
}

std::optional<ObjCReceiverTypeHint>
fieldReceiver(const BinaryImage &Image, const ObjCReceiverTypeHint &Receiver,
              uint64_t Key, bool BySlot) {
  const auto Type = receiverType(Image, Receiver);
  if (!Type || Type->IsClassMethod || Receiver.Steps.size() >= 8 ||
      (!BySlot && Key > UINT32_MAX))
    return std::nullopt;
  const auto *Ivar = receiverIvar(Image, Type->ClassName, Key, BySlot);
  if (!Ivar)
    return std::nullopt;
  auto Result = Receiver;
  Result.Steps.push_back(
      {Ivar->OffsetAddress,
       BySlot ? std::nullopt : std::optional<uint32_t>(Key),
       Image.ObjCSourceReferences.at(Ivar->OffsetAddress).Size});
  return Result;
}
} // namespace

bool objcReceiverTypeHintValid(const BinaryImage &Image,
                               const ObjCReceiverTypeHint &Receiver) {
  return receiverType(Image, Receiver).has_value();
}

std::optional<std::string>
objcReceiverInstanceClassName(const BinaryImage &Image,
                              const ObjCReceiverTypeHint &Receiver) {
  const auto Type = receiverType(Image, Receiver);
  if (!Type || Type->IsClassMethod || Type->IsProtocol)
    return std::nullopt;
  return Type->ClassName;
}

std::optional<ObjCReceiverTypeHint>
objcReceiverIvarTypeHint(const BinaryImage &Image,
                         const ObjCReceiverTypeHint &Receiver,
                         va_t OffsetSlot) {
  return fieldReceiver(Image, Receiver, OffsetSlot, true);
}

std::optional<ObjCReceiverTypeHint>
objcReceiverFieldTypeHint(const BinaryImage &Image,
                          const ObjCReceiverTypeHint &Receiver,
                          uint64_t Offset) {
  return fieldReceiver(Image, Receiver, Offset, false);
}

namespace {
std::optional<std::string> declaredReturnClass(llvm::StringRef Encoding) {
  size_t Offset = 0;
  const auto Type = parseObjCSourceType(Encoding, Offset);
  if (!Type || Type->Kind != NdTypeKind::Ptr)
    return std::nullopt;
  return objcEncodedObjectClass(Encoding.take_front(Offset));
}

std::optional<std::string> declaredReturnProtocol(llvm::StringRef Encoding) {
  size_t Offset = 0;
  const auto Type = parseObjCSourceType(Encoding, Offset);
  if (!Type || Type->Kind != NdTypeKind::Ptr)
    return std::nullopt;
  return objcEncodedObjectProtocol(Encoding.take_front(Offset));
}

ObjCReceiverDeclaration receiverDeclaration(const BinaryImage &Image,
                                            llvm::StringRef Selector,
                                            const ReceiverType &Type,
                                            ReceiverResultMode ResultMode) {
  ObjCReceiverDeclaration Result;
  bool Complete = true;
  bool KnownScope = true;
  bool CompatibleResult = true;
  auto Include = [&](const std::optional<SourceFunctionTypeHint> &Signature,
                     std::optional<std::string> ReturnClass = std::nullopt,
                     std::optional<std::string> ReturnProtocol = std::nullopt) {
    Result.HasDeclaration = true;
    if (!Signature) {
      Complete = false;
      return;
    }
    auto Hint = *Signature;
    std::string Diagnostic;
    const bool Assigned =
        ResultMode == ReceiverResultMode::ProvenNonNullSelf
            ? assignDarwinFixedSourceABI(Hint, Image.Arch, Diagnostic)
            : assignDarwinObjCSourceABI(Hint, Image.Arch, Diagnostic);
    if (!Assigned ||
        (Result.Signature && !mergeSignature(*Result.Signature, Hint))) {
      Complete = false;
      return;
    }
    if (!Result.Signature)
      Result.Signature = Hint;
    if (Hint.Origin == SourceFunctionTypeHint::OriginKind::ObjCSDK)
      Result.Signature->Origin = Hint.Origin;
    if (ReturnClass || ReturnProtocol) {
      if ((ReturnClass && ReturnProtocol) ||
          (ReturnClass && ReturnClass->empty()) ||
          (ReturnProtocol && ReturnProtocol->empty()) ||
          Hint.ReturnType->Kind != NdTypeKind::Ptr ||
          Hint.ReturnType->Size != 8 ||
          (Result.ReturnClass && Result.ReturnClass != ReturnClass) ||
          (Result.ReturnProtocol && Result.ReturnProtocol != ReturnProtocol) ||
          (Result.ReturnClass && ReturnProtocol) ||
          (Result.ReturnProtocol && ReturnClass))
        CompatibleResult = false;
      else if (ReturnClass)
        Result.ReturnClass = std::move(ReturnClass);
      else
        Result.ReturnProtocol = std::move(ReturnProtocol);
    }
  };
  // Class and protocol namespaces can share names (notably NSObject).
  using Owner = std::pair<bool, std::string>;
  std::set<Owner> Active, Visited;
  auto Visit = [&](auto &&Self, const Owner &Key) -> bool {
    if (Active.count(Key) || Visited.size() + Active.size() >= 256)
      return false;
    if (Visited.count(Key))
      return true;
    Active.insert(Key);
    const auto &[Protocol, Name] = Key;
    const auto SDK = objc::sdkReceiverDeclarations(
        Image, Name, Protocol, Type.IsClassMethod, Selector);
    Complete &= SDK.Complete;
    std::optional<std::string> Superclass = SDK.Superclass;
    std::set<Owner> Parents;
    for (const auto &Parent : SDK.Protocols)
      Parents.emplace(true, Parent);
    for (const auto &Member : SDK.Members)
      Include(Member.Signature,
              Member.ReturnsReceiverType && !Type.IsProtocol
                  ? std::optional<std::string>(Type.ClassName)
              : !Member.ReturnClass.empty()
                  ? std::optional<std::string>(Member.ReturnClass)
                  : std::nullopt,
              Member.ReturnsReceiverType && Type.IsProtocol
                  ? std::optional<std::string>(Type.ClassName)
                  : std::nullopt);
    bool Present = SDK.Present;
    if (Protocol) {
      for (const auto &Declaration : Image.ObjCProtocols) {
        if (Declaration.Name != Name)
          continue;
        Present = true;
        Complete &= Declaration.Status == "recovered";
        for (const auto &Method : Declaration.Methods)
          if (Method.IsClassMethod == Type.IsClassMethod &&
              Method.Selector == Selector)
            Include(Method.TypeHint, declaredReturnClass(Method.TypeEncoding),
                    declaredReturnProtocol(Method.TypeEncoding));
        for (va_t Address : Declaration.AdoptedProtocols) {
          std::optional<std::string> ParentName;
          for (const auto &Parent : Image.ObjCProtocols)
            if (Parent.Address == Address) {
              if (ParentName && *ParentName != Parent.Name)
                return false;
              ParentName = Parent.Name;
            }
          if (!ParentName || ParentName->empty())
            return false;
          Parents.emplace(true, *ParentName);
        }
      }
    } else {
      for (const auto &Class : Image.ObjCClasses) {
        if (Class.Name != Name)
          continue;
        Present = true;
        const bool Root = Class.RootClass &&
                          Class.InheritanceStatus == "root" &&
                          Class.SuperclassName.empty();
        if (!Root && (Class.InheritanceStatus != "resolved" ||
                      Class.SuperclassName.empty())) {
          Complete = false;
          continue;
        }
        if (Superclass && *Superclass != Class.SuperclassName)
          return false;
        Superclass = Class.SuperclassName;
      }
      for (const auto &Method : Image.ObjCMethods)
        if (Method.ClassName == Name &&
            Method.IsClassMethod == Type.IsClassMethod &&
            Method.Selector == Selector) {
          // Embedded category/manager source declarations retain object result
          // classes erased to id in runtime encodings. Match the exact local
          // declaration before carrying that class to a later message.
          const bool SDWebImageResult =
              Name == "SDWebImageManager" && !Type.IsClassMethod &&
              Selector == "processedResultForURL:options:context:" &&
              Method.TypeEncoding == "@40@0:8@16Q24@32" &&
              hasEmbeddedSDWebImageOptionsResult(Image);
          const bool WMFCalendarResult =
              Name == "NSCalendar" && !Type.IsClassMethod &&
              Selector == "wmf_components:fromDate:toDate:" &&
              hasEmbeddedWMFCalendarMethod(Image, Selector, "@40@0:8Q16@24@32",
                                           false);
          const bool WMFCalendarFactory =
              Name == "NSCalendar" && Type.IsClassMethod &&
              (Selector == "wmf_gregorianCalendar" ||
               Selector == "wmf_utcGregorianCalendar") &&
              hasEmbeddedWMFCalendarMethod(Image, Selector, "@16@0:8", true);
          Include(Method.TypeHint,
                  SDWebImageResult
                      ? std::optional<std::string>("SDWebImageOptionsResult")
                  : WMFCalendarResult
                      ? std::optional<std::string>("NSDateComponents")
                  : WMFCalendarFactory
                      ? std::optional<std::string>("NSCalendar")
                      : declaredReturnClass(Method.TypeEncoding),
                  declaredReturnProtocol(Method.TypeEncoding));
        }
      if (!Superclass)
        KnownScope = false;
      else if (!Superclass->empty())
        Parents.emplace(false, *Superclass);
    }
    for (const auto &Property : Image.ObjCProperties) {
      const bool IsProtocol =
          Property.Owner == ObjCProperty::OwnerKind::Protocol;
      if (IsProtocol != Protocol ||
          Property.IsClassProperty != Type.IsClassMethod ||
          (Protocol ? Property.OwnerName : Property.ClassName) != Name)
        continue;
      if (!Property.Getter.empty() && Property.Getter == Selector)
        Include(Property.GetterTypeHint,
                objcEncodedObjectClass(Property.TypeEncoding),
                objcEncodedObjectProtocol(Property.TypeEncoding));
      if (!Property.Setter.empty() && Property.Setter == Selector)
        Include(Property.SetterTypeHint);
    }
    KnownScope &= Present;
    for (const auto &Parent : Parents)
      if (!Self(Self, Parent))
        return false;
    Active.erase(Key);
    Visited.insert(Key);
    return true;
  };
  // Entry self is a base-class constraint. Known subclass declarations still
  // participate, whereas loading an exact class object fixes class dispatch.
  std::set<std::string> Classes;
  std::vector<std::string> Work;
  if (Type.IsProtocol) {
    if (!Visit(Visit, {true, Type.ClassName}))
      return {true, std::nullopt};
  } else {
    Classes.insert(Type.ClassName);
    Work.push_back(Type.ClassName);
  }
  if (!Type.IsProtocol && Type.IncludeSubclasses)
    while (!Work.empty()) {
      auto Name = std::move(Work.back());
      Work.pop_back();
      auto Children = objc::sdkReceiverSubclasses(Image, Name);
      for (const auto &Class : Image.ObjCClasses)
        if (Class.SuperclassName == Name)
          Children.push_back(Class.Name);
      for (const auto &Child : Children) {
        if (Child.empty() || Classes.size() >= 256)
          return {true, std::nullopt};
        if (Classes.insert(Child).second)
          Work.push_back(Child);
      }
    }
  if (!Type.IsProtocol)
    for (const auto &Class : Classes)
      if (!Visit(Visit, {false, Class}))
        return {true, std::nullopt};
  if (!Complete)
    Result.Signature.reset();
  else if (!KnownScope) {
    Result.Signature.reset();
    Result.RequiresGlobalAgreement = true;
  }
  if (!Result.Signature || !CompatibleResult) {
    Result.ReturnClass.reset();
    Result.ReturnProtocol.reset();
  }
  return Result;
}

} // namespace

ObjCReceiverDeclaration
objcReceiverSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector,
                           const ObjCReceiverTypeHint &Receiver) {
  const auto Type = receiverType(Image, Receiver);
  return Type ? receiverDeclaration(Image, Selector, *Type)
              : ObjCReceiverDeclaration{true, std::nullopt};
}

ObjCReceiverDeclaration
objcNonNilSelfSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector,
                             const ObjCReceiverTypeHint &Receiver) {
  if (Receiver.Origin != ObjCReceiverTypeHint::OriginKind::MethodEntry ||
      !Receiver.Steps.empty())
    return {};
  const auto Type = receiverType(Image, Receiver);
  return Type ? receiverDeclaration(Image, Selector, *Type,
                                    ReceiverResultMode::ProvenNonNullSelf)
              : ObjCReceiverDeclaration{true, std::nullopt};
}

ObjCReceiverDeclaration
objcSuperSourceTypeHint(const BinaryImage &Image, llvm::StringRef Selector,
                        const ObjCReceiverTypeHint &CurrentClass) {
  if (CurrentClass.Origin != ObjCReceiverTypeHint::OriginKind::ClassReference ||
      !CurrentClass.IsClassMethod || !CurrentClass.Steps.empty() ||
      !validReceiverRoot(Image, CurrentClass))
    return {};
  const ObjCClass *Class = nullptr;
  for (const auto &Candidate : Image.ObjCClasses) {
    if (Candidate.Name != CurrentClass.ClassName)
      continue;
    if (Class)
      return {};
    Class = &Candidate;
  }
  if (!Class || Class->RootClass || Class->InheritanceStatus != "resolved" ||
      Class->SuperclassName.empty())
    return {};
  return receiverDeclaration(Image, Selector,
                             ReceiverType{Class->SuperclassName, false, false});
}

std::optional<ObjCReceiverTypeHint>
objcReceiverCallResultTypeHint(const BinaryImage &Image,
                               const ObjCReceiverTypeHint &Receiver,
                               llvm::StringRef Selector) {
  if (Receiver.Steps.size() >= 8 || Selector.empty())
    return std::nullopt;
  const auto Declaration =
      objcReceiverSourceTypeHint(Image, Selector, Receiver);
  const bool DDLogFileInfoElement =
      Receiver.Steps.size() == 1 && Selector == "objectAtIndexedSubscript:" &&
      hasEmbeddedDDLogFileInfoElement(Image, Receiver) &&
      Declaration.Signature && Declaration.Signature->ReturnType &&
      Declaration.Signature->ReturnType->Kind == NdTypeKind::Ptr &&
      Declaration.Signature->ReturnType->Size == 8 &&
      !Declaration.ReturnClass && !Declaration.ReturnProtocol;
  if (!Declaration.Signature || Declaration.RequiresGlobalAgreement ||
      (!Declaration.ReturnClass && !Declaration.ReturnProtocol &&
       !DDLogFileInfoElement) ||
      (Declaration.ReturnClass && Declaration.ReturnProtocol))
    return std::nullopt;
  auto Result = Receiver;
  ObjCReceiverTypeHint::TypeStep Step;
  Step.TheKind = ObjCReceiverTypeHint::TypeStep::Kind::MessageResult;
  Step.Selector = Selector.str();
  Result.Steps.push_back(std::move(Step));
  return Result;
}

std::optional<ObjCBlockParameterContract>
objcBlockParameterContract(const BinaryImage &Image,
                           const SourceCallTypeHint &Call, unsigned Parameter) {
  if (Call.CallKind != SourceCallTypeHint::Kind::ObjCMessage ||
      Call.Selector.empty() || Parameter >= Call.Signature.Parameters.size())
    return std::nullopt;
  std::optional<SourceFunctionTypeHint> Expected;
  std::optional<ReceiverType> Type;
  if (Call.Receiver) {
    Type = receiverType(Image, *Call.Receiver);
    const auto Declaration =
        objcReceiverSourceTypeHint(Image, Call.Selector, *Call.Receiver);
    if (Type && Declaration.HasDeclaration && Declaration.Signature)
      Expected = *Declaration.Signature;
  } else if (Call.Signature.Origin ==
             SourceFunctionTypeHint::OriginKind::ObjCSDK) {
    Expected = objcSelectorSourceTypeHint(Image, Call.Selector);
  }
  auto SameDeclaration = [&](const SourceFunctionTypeHint &Left,
                             const SourceFunctionTypeHint &Right) {
    auto Merged = Left;
    return equalSourceABIs(Left, Right) && mergeSignature(Merged, Right);
  };
  if (!Expected || !SameDeclaration(Call.Signature, *Expected))
    return std::nullopt;

  auto DerivesFrom = [&](llvm::StringRef ClassName, llvm::StringRef Base) {
    std::set<std::string> Visited;
    std::string Current = ClassName.str();
    while (!Current.empty() && Visited.size() < 256) {
      if (Current == Base)
        return true;
      if (!Visited.insert(Current).second)
        return false;
      std::optional<std::string> Superclass;
      const auto SDK =
          objc::sdkReceiverDeclarations(Image, Current, false, false, {});
      if (SDK.Present) {
        if (!SDK.Complete || !SDK.Superclass)
          return false;
        Superclass = *SDK.Superclass;
      }
      for (const auto &Class : Image.ObjCClasses) {
        if (Class.Name != Current)
          continue;
        const bool Root = Class.RootClass &&
                          Class.InheritanceStatus == "root" &&
                          Class.SuperclassName.empty();
        if (!Root && (Class.InheritanceStatus != "resolved" ||
                      Class.SuperclassName.empty()))
          return false;
        if (Superclass && *Superclass != Class.SuperclassName)
          return false;
        Superclass = Class.SuperclassName;
      }
      if (!Superclass || Superclass->empty())
        return false;
      Current = *Superclass;
    }
    return false;
  };

  // FLAnimatedImage's logging implementation invokes the supplied string
  // producer before returning and never retains it. Its public source and the
  // embedded class method agree on this exact declaration. Require an exact
  // class reference so a subclass override cannot inherit the lifetime rule.
  if (Image.Arch == Arch::AArch64 && Type && Type->IsClassMethod &&
      !Type->IncludeSubclasses && !Type->IsProtocol &&
      Type->ClassName == "FLAnimatedImage" &&
      Call.Selector == "logStringFromBlock:withLevel:" && Parameter == 2) {
    const ObjCClass *Owner = nullptr;
    for (const auto &Class : Image.ObjCClasses)
      if (Class.Name == "FLAnimatedImage") {
        if (Owner)
          return std::nullopt;
        Owner = &Class;
      }
    if (!Owner || !Owner->Address)
      return std::nullopt;
    const ObjCMethod *Method = nullptr;
    for (const auto &Candidate : Image.ObjCMethods)
      if (Candidate.ClassName == "FLAnimatedImage" &&
          Candidate.Selector == "logStringFromBlock:withLevel:" &&
          Candidate.IsClassMethod) {
        if (Method || Candidate.ClassAddress != Owner->Address ||
            Candidate.CategoryAddress || !Candidate.CategoryName.empty() ||
            !Candidate.MetadataAddress ||
            Candidate.TypeEncoding != "v32@0:8@?16Q24" ||
            !objcMethodHasSourceBody(Candidate) ||
            !Image.isCodeAddress(Candidate.Implementation))
          return std::nullopt;
        Method = &Candidate;
      }
    if (!Method)
      return std::nullopt;
    auto Parent = parseObjCMethodEncoding(Call.Selector, Method->TypeEncoding);
    std::string Error;
    auto Callback = parseObjCBlockSignature("@8@?0", Image.Arch, Error);
    if (!Parent || !Callback ||
        !assignDarwinObjCSourceABI(*Parent, Image.Arch, Error) ||
        !SameDeclaration(*Expected, *Parent))
      return std::nullopt;
    return ObjCBlockParameterContract{
        std::move(*Callback), ObjCBlockParameterContract::Lifetime::NonEscaping};
  }

  // Mantle's transformer factories retain both callbacks in the constructed
  // transformer using copied block properties. Limit this rule to the exact
  // embedded class methods and their published block ABI.
  if (Image.Arch == Arch::AArch64 && Type && Type->IsClassMethod &&
      !Type->IncludeSubclasses && !Type->IsProtocol &&
      Type->ClassName == "MTLValueTransformer" &&
      (Call.Selector == "transformerUsingForwardBlock:" ||
       Call.Selector == "transformerUsingForwardBlock:reverseBlock:") &&
      (Parameter == 2 ||
       (Call.Selector == "transformerUsingForwardBlock:reverseBlock:" &&
        Parameter == 3))) {
    const llvm::StringRef Encoding =
        Call.Selector == "transformerUsingForwardBlock:"
            ? "@24@0:8@?16"
            : "@32@0:8@?16@?24";
    const ObjCClass *Owner = nullptr;
    for (const auto &Class : Image.ObjCClasses)
      if (Class.Name == "MTLValueTransformer") {
        if (Owner)
          return std::nullopt;
        Owner = &Class;
      }
    if (!Owner || !Owner->Address)
      return std::nullopt;
    const ObjCMethod *Method = nullptr;
    for (const auto &Candidate : Image.ObjCMethods)
      if (Candidate.ClassName == "MTLValueTransformer" &&
          Candidate.Selector == Call.Selector && Candidate.IsClassMethod) {
        if (Method || Candidate.ClassAddress != Owner->Address ||
            Candidate.CategoryAddress || !Candidate.CategoryName.empty() ||
            !Candidate.MetadataAddress || Candidate.TypeEncoding != Encoding ||
            !objcMethodHasSourceBody(Candidate) ||
            !Image.isCodeAddress(Candidate.Implementation))
          return std::nullopt;
        Method = &Candidate;
      }
    if (!Method)
      return std::nullopt;
    auto Parent = parseObjCMethodEncoding(Call.Selector, Encoding);
    std::string Error;
    auto Callback = parseObjCBlockSignature("@32@?0@8^B16^@24",
                                            Image.Arch, Error);
    if (!Parent || !Callback ||
        !assignDarwinObjCSourceABI(*Parent, Image.Arch, Error) ||
        !SameDeclaration(*Expected, *Parent))
      return std::nullopt;
    return ObjCBlockParameterContract{
        std::move(*Callback), ObjCBlockParameterContract::Lifetime::Copied};
  }

  // Wikipedia iOS declares AsyncBlockOperation.init(asyncBlock:) with an
  // escaping Swift closure. The Objective-C bridge therefore owns a copy of
  // the incoming block after this initializer returns.
  if (Image.Arch == Arch::AArch64 && Type && !Type->IsClassMethod &&
      !Type->IsProtocol && Type->ClassName == "WMFAsyncBlockOperation" &&
      Call.Selector == "initWithAsyncBlock:" && Parameter == 2) {
    const ObjCClass *Owner = nullptr;
    for (const auto &Class : Image.ObjCClasses)
      if (Class.Name == "WMFAsyncBlockOperation") {
        if (Owner)
          return std::nullopt;
        Owner = &Class;
      }
    if (!Owner || !Owner->Address ||
        Owner->SuperclassName != "WMFAsyncOperation" ||
        Owner->InheritanceStatus != "resolved")
      return std::nullopt;
    const ObjCMethod *Method = nullptr;
    for (const auto &Candidate : Image.ObjCMethods)
      if (Candidate.ClassName == Owner->Name &&
          Candidate.Selector == Call.Selector && !Candidate.IsClassMethod) {
        if (Method || Candidate.ClassAddress != Owner->Address ||
            Candidate.CategoryAddress || !Candidate.CategoryName.empty() ||
            !Candidate.MetadataAddress ||
            Candidate.TypeEncoding != "@24@0:8@?16" ||
            !objcMethodHasSourceBody(Candidate) ||
            !Image.isCodeAddress(Candidate.Implementation))
          return std::nullopt;
        Method = &Candidate;
      }
    if (!Method)
      return std::nullopt;
    auto Parent = parseObjCMethodEncoding(Call.Selector, Method->TypeEncoding);
    std::string Error;
    auto Callback = parseObjCBlockSignature(
        "v16@?0@\"WMFAsyncBlockOperation\"8", Image.Arch, Error);
    if (!Parent || !Callback ||
        !assignDarwinObjCSourceABI(*Parent, Image.Arch, Error) ||
        !SameDeclaration(*Expected, *Parent))
      return std::nullopt;
    return ObjCBlockParameterContract{
        std::move(*Callback), ObjCBlockParameterContract::Lifetime::Copied};
  }

  // WMFBlocksKit.swift imports these collection callbacks as @escaping.
  // Match the embedded Swift-extension methods, owner and complete callback
  // ABI before treating an Objective-C stack literal as copied by the bridge.
  struct WMFCollectionBlock {
    const char *Owner;
    const char *Selector;
    const char *Parent;
    const char *Callback;
    unsigned Parameter;
  };
  static constexpr WMFCollectionBlock WMFCollectionBlocks[] = {
      {"NSArray", "wmf_map:", "@24@0:8@?16", "@16@?0@8", 2},
      {"NSArray", "wmf_select:", "@24@0:8@?16", "B16@?0@8", 2},
      {"NSArray", "wmf_match:", "@24@0:8@?16", "B16@?0@8", 2},
      {"NSArray", "wmf_reduce:withBlock:", "@32@0:8@16@?24",
       "@24@?0@8@16", 3},
      {"NSSet", "wmf_map:", "@24@0:8@?16", "@16@?0@8", 2},
      {"NSSet", "wmf_select:", "@24@0:8@?16", "B16@?0@8", 2},
      {"NSSet", "wmf_match:", "@24@0:8@?16", "B16@?0@8", 2},
      {"NSSet", "wmf_reduce:withBlock:", "@32@0:8@16@?24",
       "@24@?0@8@16", 3},
      {"NSDictionary", "wmf_map:", "@24@0:8@?16", "@24@?0@8@16", 2},
      {"NSDictionary", "wmf_select:", "@24@0:8@?16", "B24@?0@8@16", 2},
      {"NSDictionary", "wmf_match:", "@24@0:8@?16", "B24@?0@8@16", 2},
      {"NSDictionary", "wmf_reduce:withBlock:", "@32@0:8@16@?24",
       "@32@?0@8@16@24", 3},
  };
  if (Image.Arch == Arch::AArch64 && Type && !Type->IsClassMethod &&
      !Type->IsProtocol)
    for (const auto &D : WMFCollectionBlocks) {
      if (Call.Selector != D.Selector || Parameter != D.Parameter ||
          !DerivesFrom(Type->ClassName, D.Owner))
        continue;
      const ObjCMethod *Method = nullptr;
      for (const auto &Candidate : Image.ObjCMethods)
        if (Candidate.ClassName == D.Owner &&
            Candidate.Selector == D.Selector && !Candidate.IsClassMethod) {
          if (Method || !Candidate.CategoryAddress ||
              !Candidate.MetadataAddress || Candidate.TypeEncoding != D.Parent ||
              !objcMethodHasSourceBody(Candidate) ||
              !Image.isCodeAddress(Candidate.Implementation))
            return std::nullopt;
          Method = &Candidate;
        }
      if (!Method)
        return std::nullopt;
      auto Parent = parseObjCMethodEncoding(D.Selector, D.Parent);
      std::string Error;
      auto Callback = parseObjCBlockSignature(D.Callback, Image.Arch, Error);
      if (!Parent || !Callback ||
          !assignDarwinObjCSourceABI(*Parent, Image.Arch, Error) ||
          !SameDeclaration(*Expected, *Parent))
        return std::nullopt;
      return ObjCBlockParameterContract{
          std::move(*Callback), ObjCBlockParameterContract::Lifetime::Copied};
    }

  struct Declaration {
    const char *Selector;
    const char *AArch64Parent;
    const char *X64Parent;
    unsigned Parameter;
    const char *AArch64Callback;
    const char *X64Callback;
    const char *Owner;
    bool Copied = false;
  };
  // Compiler-derived from public SDK 15.5 block method parameters. The
  // nonescaping rows carry NS_NOESCAPE; copied rows are audited asynchronous
  // or retained-work APIs. A null callback records architecture profiles
  // where the macOS and iOS declarations disagree; no implementation is
  // included.
  static constexpr Declaration Declarations[] = {
      {"enumerateKeysAndObjectsUsingBlock:", "v24@0:8@?16", "v24@0:8@?16", 2,
       "v32@?0@8@16^B24", nullptr, "NSDictionary"},
      {"enumerateObjectsUsingBlock:", "v24@0:8@?16", "v24@0:8@?16", 2,
       "v32@?0@8Q16^B24", "v32@?0@8Q16^B24", "NSArray"},
      {"enumerateObjectsUsingBlock:", "v24@0:8@?16", "v24@0:8@?16", 2,
       "v32@?0@8Q16^B24", "v32@?0@8Q16^B24", "NSOrderedSet"},
      {"enumerateObjectsUsingBlock:", "v24@0:8@?16", "v24@0:8@?16", 2,
       "v24@?0@8^B16", "v24@?0@8^B16", "NSSet"},
      {"enumerateObjectsWithOptions:usingBlock:", "v32@0:8Q16@?24",
       "v32@0:8Q16@?24", 3, "v32@?0@8Q16^B24", nullptr, "NSArray"},
      {"enumerateObjectsWithOptions:usingBlock:", "v32@0:8Q16@?24",
       "v32@0:8Q16@?24", 3, "v32@?0@8Q16^B24", nullptr, "NSOrderedSet"},
      {"enumerateObjectsWithOptions:usingBlock:", "v32@0:8Q16@?24",
       "v32@0:8Q16@?24", 3, "v24@?0@8^B16", nullptr, "NSSet"},
      {"indexesOfObjectsPassingTest:", "@24@0:8@?16", "@24@0:8@?16", 2,
       "B32@?0@8Q16^B24", "B32@?0@8Q16^B24", "NSArray"},
      {"indexesOfObjectsPassingTest:", "@24@0:8@?16", "@24@0:8@?16", 2,
       "B32@?0@8Q16^B24", "B32@?0@8Q16^B24", "NSOrderedSet"},
      {"keysSortedByValueWithOptions:usingComparator:", "@32@0:8Q16@?24",
       "@32@0:8Q16@?24", 3, "q24@?0@8@16", "q24@?0@8@16", "NSDictionary"},
      {"performBlockAndWait:", "v24@0:8@?16", "v24@0:8@?16", 2, "v8@?0",
       "v8@?0", "NSManagedObjectContext"},
      {"performBlockAndWait:", "v24@0:8@?16", "v24@0:8@?16", 2, "v8@?0",
       "v8@?0", "NSPersistentStoreCoordinator"},
      // Both Core Data performBlock: methods enqueue the callback after the
      // message returns, so a stack literal must be copied for that lifetime.
      {"performBlock:", "v24@0:8@?16", "v24@0:8@?16", 2, "v8@?0", "v8@?0",
       "NSManagedObjectContext", true},
      {"performBlock:", "v24@0:8@?16", "v24@0:8@?16", 2, "v8@?0", "v8@?0",
       "NSPersistentStoreCoordinator", true},
      // A store description may request asynchronous addition; Swift imports
      // this completion as escaping, so the stack block must be copied.
      {"loadPersistentStoresWithCompletionHandler:", "v24@0:8@?16",
       "v24@0:8@?16", 2, "v24@?0@8@16", "v24@?0@8@16", "NSPersistentContainer",
       true},
      // These APIs retain work beyond the message return. The authorization
      // callback's BOOL encoding differs between macOS and iOS x86-64.
      {"blockOperationWithBlock:", "@24@0:8@?16", "@24@0:8@?16", 2, "v8@?0",
       "v8@?0", "NSBlockOperation", true},
      {"reverseGeocodeLocation:completionHandler:", "v32@0:8@16@?24",
       "v32@0:8@16@?24", 3, "v24@?0@8@16", "v24@?0@8@16", "CLGeocoder", true},
      {"getNotificationSettingsWithCompletionHandler:", "v24@0:8@?16",
       "v24@0:8@?16", 2, "v16@?0@8", "v16@?0@8", "UNUserNotificationCenter",
       true},
      {"requestAuthorizationWithOptions:completionHandler:", "v32@0:8Q16@?24",
       "v32@0:8Q16@?24", 3, "v20@?0B8@12", nullptr, "UNUserNotificationCenter",
       true},
      {"sortedArrayUsingComparator:", "@24@0:8@?16", "@24@0:8@?16", 2,
       "q24@?0@8@16", "q24@?0@8@16", "NSArray"},
      {"sortedArrayUsingComparator:", "@24@0:8@?16", "@24@0:8@?16", 2,
       "q24@?0@8@16", "q24@?0@8@16", "NSOrderedSet"},
      {"enumerateMatchesInString:options:range:usingBlock:",
       "v56@0:8@16Q24{_NSRange=QQ}32@?48", "v56@0:8@16Q24{_NSRange=QQ}32@?48",
       5, "v32@?0@8Q16^B24", nullptr, "NSRegularExpression"},
      {"imageWithActions:", "@24@0:8@?16", "@24@0:8@?16", 2, "v16@?0@8",
       "v16@?0@8", "UIGraphicsImageRenderer"},
  };
  std::optional<ObjCBlockParameterContract> Result;
  for (const auto &D : Declarations) {
    if (Call.Selector != D.Selector || Parameter != D.Parameter)
      continue;
    if (D.Copied && !Type)
      continue;
    const auto *ParentEncoding =
        Image.Arch == Arch::AArch64 ? D.AArch64Parent : D.X64Parent;
    const auto *CallbackEncoding =
        Image.Arch == Arch::AArch64 ? D.AArch64Callback : D.X64Callback;
    if (!ParentEncoding || !CallbackEncoding)
      continue;
    auto Parent = parseObjCMethodEncoding(D.Selector, ParentEncoding);
    std::string Error;
    auto Callback =
        parseObjCBlockSignature(CallbackEncoding, Image.Arch, Error);
    if (Parent)
      Parent->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
    if (!Parent || !Callback ||
        !assignDarwinObjCSourceABI(*Parent, Image.Arch, Error) ||
        !SameDeclaration(*Expected, *Parent))
      return std::nullopt;
    if (Type && !DerivesFrom(Type->ClassName, D.Owner))
      continue;
    const auto Lifetime =
        D.Copied ? ObjCBlockParameterContract::Lifetime::Copied
                 : ObjCBlockParameterContract::Lifetime::NonEscaping;
    if (Result && (Result->Storage != Lifetime ||
                   !SameDeclaration(Result->Signature, *Callback)))
      return std::nullopt;
    Result = ObjCBlockParameterContract{std::move(*Callback), Lifetime};
  }
  return Result;
}

std::optional<SourceFunctionTypeHint>
objcNonEscapingBlockSignature(const BinaryImage &Image,
                              const SourceCallTypeHint &Call,
                              unsigned Parameter) {
  auto Contract = objcBlockParameterContract(Image, Call, Parameter);
  return Contract && Contract->Storage ==
                         ObjCBlockParameterContract::Lifetime::NonEscaping
             ? std::optional<SourceFunctionTypeHint>(
                   std::move(Contract->Signature))
             : std::nullopt;
}

std::optional<ObjCFormatDeclaration>
objcSelectorFormatDeclaration(const BinaryImage &Image,
                              llvm::StringRef Selector) {
  const auto *Catalog = frameworkDeclarations(Image.Arch);
  if (!Catalog)
    return std::nullopt;
  const auto &Foundation = Catalog->at("Foundation");
  if (!usesFramework(Image, Foundation))
    return std::nullopt;
  const auto Ordinary = Foundation.Selectors.find(Selector.str());
  if (Ordinary == Foundation.Selectors.end() || Ordinary->second)
    return std::nullopt;
  static constexpr struct {
    const char *Selector;
    const char *AArch64;
    const char *X64;
    unsigned FormatParameter;
    unsigned FixedCount;
    SourceCallTypeHint::FormatSyntax Syntax =
        SourceCallTypeHint::FormatSyntax::NSString;
  } Formats[] = {
#include "ObjCFormatDeclarations.inc"
#include "ObjCPredicateDeclarations.inc"
  };
  std::optional<ObjCFormatDeclaration> Result;
  for (const auto &D : Formats) {
    if (D.Selector != Selector)
      continue;
    // More than one declaration must never be resolved by visitation order.
    if (Result)
      return std::nullopt;
    auto Signature = parseObjCMethodEncoding(
        Selector, Image.Arch == Arch::AArch64 ? D.AArch64 : D.X64);
    std::string Diagnostic;
    if (!Signature || Signature->Parameters.size() != D.FixedCount ||
        D.FormatParameter >= D.FixedCount ||
        Signature->Parameters[D.FormatParameter].Type->Kind != NdTypeKind::Ptr)
      return std::nullopt;
    Signature->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
    if (!assignDarwinObjCSourceABI(*Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    Signature = selectorSourceTypeHint(Image, Selector, &*Signature);
    if (!Signature)
      return std::nullopt;
    Result = ObjCFormatDeclaration{std::move(*Signature), D.FormatParameter,
                                   D.Syntax};
  }
  return Result;
}
} // namespace neverd
