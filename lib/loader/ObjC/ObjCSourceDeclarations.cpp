#include "neverd/loader/ObjC/ObjCSourceDeclarations.h"

#include "ObjCReceiverDeclarations.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCBlocks.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"

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
       *RequiredType != NdTypeKind::Ptr &&
       *RequiredType != NdTypeKind::Float))
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

std::optional<SourceFunctionTypeHint>
objcSelectorSourceTypeHintForArgumentTypeUse(
    const BinaryImage &Image, llvm::StringRef Selector,
    const SourceCallTypeHint::SelectorArgumentTypeEvidence &Evidence) {
  if (!Evidence.MethodEntry || Evidence.Parameter < 2)
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
  if (!Source || !isObjCSelectorArgumentEvidenceType(
                     Source->Type, Evidence.ConsumedAsObject))
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

std::optional<SourceFunctionTypeHint>
objcMethodForwardingSourceTypeHint(
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
  auto Result =
      objcMethodForwardingSourceTypeHint(Image, Selector, Evidence);
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
  return (Type->Pointee->Kind == NdTypeKind::Ptr &&
          Type->Pointee->Size == 8) ||
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

namespace {
bool validReceiverRoot(const BinaryImage &Image,
                       const ObjCReceiverTypeHint &Receiver) {
  if (!Receiver.Address || Receiver.ClassName.empty() ||
      Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64))
    return false;
  switch (Receiver.Origin) {
  case ObjCReceiverTypeHint::OriginKind::MethodEntry: {
    const auto Expected = objcMethodReceiverTypeHint(Image, Receiver.Address);
    return Expected && Expected->ClassName == Receiver.ClassName &&
           Expected->IsClassMethod == Receiver.IsClassMethod;
  }
  case ObjCReceiverTypeHint::OriginKind::ClassReference: {
    const auto Found = Image.ObjCSourceReferences.find(Receiver.Address);
    return Receiver.IsClassMethod &&
           Found != Image.ObjCSourceReferences.end() &&
           Found->second.Address == Receiver.Address &&
           Found->second.TheKind == ObjCSourceReference::Kind::Class &&
           Found->second.Size == 8 && Found->second.Name == Receiver.ClassName;
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
          !objcEncodedObjectClass(Ivar.TypeEncoding))
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
};

enum class ReceiverResultMode { ObjCNilDispatch, ProvenNonNullSelf };

ObjCReceiverDeclaration receiverDeclaration(
    const BinaryImage &Image, llvm::StringRef Selector,
    const ReceiverType &Type,
    ReceiverResultMode ResultMode = ReceiverResultMode::ObjCNilDispatch);

std::optional<ReceiverType> receiverType(const BinaryImage &Image,
                                         const ObjCReceiverTypeHint &Receiver) {
  if (Receiver.Steps.size() > 8 || !validReceiverRoot(Image, Receiver))
    return std::nullopt;
  ReceiverType Result{Receiver.ClassName, Receiver.IsClassMethod,
                      Receiver.Origin ==
                          ObjCReceiverTypeHint::OriginKind::MethodEntry};
  for (const auto &Access : Receiver.Steps) {
    if (Access.TheKind == ObjCReceiverTypeHint::TypeStep::Kind::MessageResult) {
      if (Access.Selector.empty() || Access.OffsetSlot || Access.ByteOffset ||
          Access.OffsetWidth)
        return std::nullopt;
      const auto Declaration =
          receiverDeclaration(Image, Access.Selector, Result);
      if (!Declaration.Signature || Declaration.RequiresGlobalAgreement ||
          !Declaration.ReturnClass)
        return std::nullopt;
      Result = {*Declaration.ReturnClass, false, true};
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
    if (!ClassName)
      return std::nullopt;
    Result.ClassName = *ClassName;
    Result.IncludeSubclasses = true;
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

ObjCReceiverDeclaration receiverDeclaration(const BinaryImage &Image,
                                            llvm::StringRef Selector,
                                            const ReceiverType &Type,
                                            ReceiverResultMode ResultMode) {
  ObjCReceiverDeclaration Result;
  bool Complete = true;
  bool KnownScope = true;
  bool CompatibleResult = true;
  auto Include = [&](const std::optional<SourceFunctionTypeHint> &Signature,
                     std::optional<std::string> ReturnClass = std::nullopt) {
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
    if (ReturnClass) {
      if (ReturnClass->empty() || Hint.ReturnType->Kind != NdTypeKind::Ptr ||
          Hint.ReturnType->Size != 8 ||
          (Result.ReturnClass && Result.ReturnClass != ReturnClass))
        CompatibleResult = false;
      else
        Result.ReturnClass = std::move(ReturnClass);
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
              Member.ReturnsReceiverType
                  ? std::optional<std::string>(Type.ClassName)
              : !Member.ReturnClass.empty()
                  ? std::optional<std::string>(Member.ReturnClass)
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
            Include(Method.TypeHint, declaredReturnClass(Method.TypeEncoding));
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
            Method.Selector == Selector)
          Include(Method.TypeHint, declaredReturnClass(Method.TypeEncoding));
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
                objcEncodedObjectClass(Property.TypeEncoding));
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
  std::set<std::string> Classes{Type.ClassName};
  std::vector<std::string> Work{Type.ClassName};
  if (Type.IncludeSubclasses)
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
  for (const auto &Class : Classes)
    if (!Visit(Visit, {false, Class}))
      return {true, std::nullopt};
  if (!Complete)
    Result.Signature.reset();
  else if (!KnownScope) {
    Result.Signature.reset();
    Result.RequiresGlobalAgreement = true;
  }
  if (!Result.Signature || !CompatibleResult)
    Result.ReturnClass.reset();
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
  if (!Declaration.Signature || Declaration.RequiresGlobalAgreement ||
      !Declaration.ReturnClass)
    return std::nullopt;
  auto Result = Receiver;
  ObjCReceiverTypeHint::TypeStep Step;
  Step.TheKind = ObjCReceiverTypeHint::TypeStep::Kind::MessageResult;
  Step.Selector = Selector.str();
  Result.Steps.push_back(std::move(Step));
  return Result;
}

std::optional<SourceFunctionTypeHint>
objcNonEscapingBlockSignature(const BinaryImage &Image,
                              const SourceCallTypeHint &Call,
                              unsigned Parameter) {
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

  struct Declaration {
    const char *Selector;
    const char *AArch64Parent;
    const char *X64Parent;
    unsigned Parameter;
    const char *AArch64Callback;
    const char *X64Callback;
    const char *Owner;
  };
  // Compiler-derived from Foundation SDK 15.5 public NS_NOESCAPE method
  // parameters. Parent and callback ABIs agree across the corresponding
  // macOS/iOS and simulator/device profiles; no implementation is included.
  static constexpr Declaration Declarations[] = {
      {"enumerateObjectsUsingBlock:", "v24@0:8@?16", "v24@0:8@?16", 2,
       "v32@?0@8Q16^B24", "v32@?0@8Q16^B24", "NSArray"},
      {"enumerateObjectsUsingBlock:", "v24@0:8@?16", "v24@0:8@?16", 2,
       "v32@?0@8Q16^B24", "v32@?0@8Q16^B24", "NSOrderedSet"},
      {"enumerateObjectsUsingBlock:", "v24@0:8@?16", "v24@0:8@?16", 2,
       "v24@?0@8^B16", "v24@?0@8^B16", "NSSet"},
      {"indexesOfObjectsPassingTest:", "@24@0:8@?16", "@24@0:8@?16", 2,
       "B32@?0@8Q16^B24", "B32@?0@8Q16^B24", "NSArray"},
      {"indexesOfObjectsPassingTest:", "@24@0:8@?16", "@24@0:8@?16", 2,
       "B32@?0@8Q16^B24", "B32@?0@8Q16^B24", "NSOrderedSet"},
  };
  std::optional<SourceFunctionTypeHint> Result;
  for (const auto &D : Declarations) {
    if (Call.Selector != D.Selector || Parameter != D.Parameter)
      continue;
    auto Parent = parseObjCMethodEncoding(
        D.Selector,
        Image.Arch == Arch::AArch64 ? D.AArch64Parent : D.X64Parent);
    std::string Error;
    auto Callback = parseObjCBlockSignature(
        Image.Arch == Arch::AArch64 ? D.AArch64Callback : D.X64Callback,
        Image.Arch, Error);
    if (Parent)
      Parent->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
    if (!Parent || !Callback ||
        !assignDarwinObjCSourceABI(*Parent, Image.Arch, Error) ||
        !SameDeclaration(*Expected, *Parent))
      return std::nullopt;
    if (Type && !DerivesFrom(Type->ClassName, D.Owner))
      continue;
    if (Result && !SameDeclaration(*Result, *Callback))
      return std::nullopt;
    Result = std::move(*Callback);
  }
  return Result;
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
