#include "ObjCReceiverDeclarations.h"

#include "../MachO/DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"

#include <map>
#include <tuple>

namespace neverd::objc {
namespace {
using OwnerKey = std::tuple<std::string, std::string, std::string>;
using MemberKey = std::pair<bool, std::string>;
struct Owner {
  std::optional<std::string> Parents;
  std::map<MemberKey, std::vector<ReceiverMemberDeclaration>> Members;
};
struct Framework {
  std::string Modules;
  std::map<OwnerKey, Owner> Owners;
};
using Catalog = std::map<std::string, Framework>;

Catalog buildCatalog(Arch Architecture) {
  Catalog Result;
  auto AddOwner = [&](const char *Provider, const char *Modules,
                      const char *Kind, const char *Name, const char *Category,
                      const char *Arm, const char *X64) {
    const char *Parents = Architecture == Arch::AArch64 ? Arm : X64;
    if (!Parents)
      return;
    auto &Framework = Result[Provider];
    Framework.Modules = Modules;
    auto &Owner = Framework.Owners[{Kind, Name, Category}];
    if (Owner.Parents && *Owner.Parents != Parents)
      Owner.Parents = "!";
    else
      Owner.Parents = Parents;
  };
  auto AddMember = [&](const char *Provider, const char *Modules,
                       const char *Kind, const char *Name, const char *Category,
                       bool ClassMethod, const char *Selector, const char *Arm,
                       const char *ArmClass, bool ArmSelf, const char *X64,
                       const char *X64Class, bool X64Self) {
    const bool A64 = Architecture == Arch::AArch64;
    const char *Encoding = A64 ? Arm : X64;
    if (!Encoding)
      return;
    auto &Framework = Result[Provider];
    Framework.Modules = Modules;
    auto Signature = parseObjCMethodEncoding(Selector, Encoding);
    std::string Diagnostic;
    if (Signature) {
      Signature->Origin = SourceFunctionTypeHint::OriginKind::ObjCSDK;
      // Retain fixed-ABI indirect results in the owner-qualified catalog.
      // Ordinary Objective-C lookup still rejects them; a call-site proof of
      // non-null self and private result storage may request them separately.
      if (!assignDarwinFixedSourceABI(*Signature, Architecture, Diagnostic))
        Signature.reset();
    }
    Framework.Owners[{Kind, Name, Category}]
        .Members[{ClassMethod, Selector}]
        .push_back({std::move(Signature), A64 ? ArmClass : X64Class,
                    A64 ? ArmSelf : X64Self});
  };
  auto AddOutParameter = [&](const char *Provider, const char *Modules,
                             const char *Kind, const char *Name,
                             const char *Category, bool ClassMethod,
                             const char *Selector, unsigned Parameter,
                             const char *ArmClass, const char *X64Class) {
    const char *Class = Architecture == Arch::AArch64 ? ArmClass : X64Class;
    if (!Class || !*Class)
      return;
    auto &Framework = Result[Provider];
    Framework.Modules = Modules;
    auto &Members = Framework.Owners[{Kind, Name, Category}]
                        .Members[{ClassMethod, Selector}];
    for (auto &Member : Members)
      Member.OutParameterClasses[Parameter] = Class;
  };
  static constexpr struct {
    const char *Provider, *Modules, *Kind, *Name, *Category, *Arm, *X64;
  } Owners[] = {
#define ND_OBJC_OWNER(...) {__VA_ARGS__},
#define ND_OBJC_MEMBER(...)
#define ND_OBJC_OUT_PARAMETER(...)
#include "ObjCIOSReceiverDeclarations.inc"
#include "ObjCReceiverDeclarations.inc"
#undef ND_OBJC_OUT_PARAMETER
#undef ND_OBJC_MEMBER
#undef ND_OBJC_OWNER
  };
  static constexpr struct {
    const char *Provider, *Modules, *Kind, *Name, *Category;
    bool ClassMethod;
    const char *Selector, *Arm, *ArmClass;
    bool ArmSelf;
    const char *X64, *X64Class;
    bool X64Self;
  } Members[] = {
#define ND_OBJC_OWNER(...)
#define ND_OBJC_MEMBER(...) {__VA_ARGS__},
#define ND_OBJC_OUT_PARAMETER(...)
#include "ObjCIOSReceiverDeclarations.inc"
#include "ObjCReceiverDeclarations.inc"
#undef ND_OBJC_OUT_PARAMETER
#undef ND_OBJC_MEMBER
#undef ND_OBJC_OWNER
  };
  static constexpr struct {
    const char *Provider, *Modules, *Kind, *Name, *Category;
    bool ClassMethod;
    const char *Selector;
    unsigned Parameter;
    const char *ArmClass, *X64Class;
  } OutParameters[] = {
#define ND_OBJC_OWNER(...)
#define ND_OBJC_MEMBER(...)
#define ND_OBJC_OUT_PARAMETER(...) {__VA_ARGS__},
#include "ObjCIOSReceiverDeclarations.inc"
#include "ObjCReceiverDeclarations.inc"
#undef ND_OBJC_OUT_PARAMETER
#undef ND_OBJC_MEMBER
#undef ND_OBJC_OWNER
  };
  for (const auto &D : Owners)
    AddOwner(D.Provider, D.Modules, D.Kind, D.Name, D.Category, D.Arm, D.X64);
  for (const auto &D : Members)
    AddMember(D.Provider, D.Modules, D.Kind, D.Name, D.Category, D.ClassMethod,
              D.Selector, D.Arm, D.ArmClass, D.ArmSelf, D.X64, D.X64Class,
              D.X64Self);
  for (const auto &D : OutParameters)
    AddOutParameter(D.Provider, D.Modules, D.Kind, D.Name, D.Category,
                    D.ClassMethod, D.Selector, D.Parameter, D.ArmClass,
                    D.X64Class);
  return Result;
}

const Catalog *catalog(Arch Architecture) {
  if (Architecture == Arch::AArch64) {
    static const auto Result = buildCatalog(Arch::AArch64);
    return &Result;
  }
  if (Architecture == Arch::X64) {
    static const auto Result = buildCatalog(Arch::X64);
    return &Result;
  }
  return nullptr;
}

bool active(const BinaryImage &Image, llvm::StringRef Modules) {
  while (!Modules.empty()) {
    const auto [Module, Rest] = Modules.split('|');
    if (llvm::is_contained(Image.DynInfo.NeededLibs, Module))
      return true;
    Modules = Rest;
  }
  return false;
}
} // namespace

bool sdkClassImportProvider(Arch Architecture, llvm::StringRef Class,
                            llvm::StringRef Module) {
  const auto *Catalog = catalog(Architecture);
  if (!Catalog || Class.empty() || Module.empty())
    return false;
  unsigned Matches = 0;
  for (const auto &[Provider, Framework] : *Catalog) {
    const auto Owner = Framework.Owners.find({"class", Class.str(), ""});
    if (Owner == Framework.Owners.end())
      continue;
    if (!Owner->second.Parents || *Owner->second.Parents == "!" ||
        !darwinExportModuleMatches(Framework.Modules, Module))
      return false;
    ++Matches;
  }
  return Matches == 1;
}

ReceiverDeclarations sdkReceiverDeclarations(const BinaryImage &Image,
                                             llvm::StringRef Name,
                                             bool Protocol, bool ClassMethod,
                                             llvm::StringRef Selector) {
  ReceiverDeclarations Result;
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64)
    return Result;
  const auto *Catalog = catalog(Image.Arch);
  if (!Catalog)
    return Result;
  for (const auto &[Provider, Framework] : *Catalog) {
    if (!active(Image, Framework.Modules))
      continue;
    for (llvm::StringRef Scope : {"class", "category", "protocol"}) {
      if ((Scope == "protocol") != Protocol)
        continue;
      auto It = Framework.Owners.lower_bound({Scope.str(), Name.str(), ""});
      for (; It != Framework.Owners.end(); ++It) {
        const auto &[Kind, OwnerName, Category] = It->first;
        if (Kind != Scope || OwnerName != Name)
          break;
        const auto &Owner = It->second;
        Result.Present = true;
        if (!Owner.Parents || *Owner.Parents == "!") {
          Result.Complete = false;
          continue;
        }
        llvm::StringRef Parents(*Owner.Parents);
        std::string Superclass;
        while (!Parents.empty()) {
          auto [Parent, Rest] = Parents.split('|');
          if (Parent.consume_front("C:") && !Parent.empty() &&
              Kind == "class" && Superclass.empty())
            Superclass = Parent.str();
          else if (Parent.consume_front("P:") && !Parent.empty())
            Result.Protocols.push_back(Parent.str());
          else
            Result.Complete = false;
          Parents = Rest;
        }
        if (Kind == "class") {
          if (Result.Superclass && *Result.Superclass != Superclass)
            Result.Complete = false;
          else
            Result.Superclass = std::move(Superclass);
        }
        auto Member = Owner.Members.find({ClassMethod, Selector.str()});
        if (Member != Owner.Members.end())
          Result.Members.insert(Result.Members.end(), Member->second.begin(),
                                Member->second.end());
      }
    }
  }
  return Result;
}

std::vector<std::string> sdkReceiverSubclasses(const BinaryImage &Image,
                                               llvm::StringRef Name) {
  std::vector<std::string> Result;
  const auto *Catalog = catalog(Image.Arch);
  if (!Catalog)
    return Result;
  const auto Parent = ("C:" + Name).str();
  for (const auto &[Provider, Framework] : *Catalog) {
    if (!active(Image, Framework.Modules))
      continue;
    for (const auto &[Key, Owner] : Framework.Owners) {
      const auto &[Kind, Class, Category] = Key;
      if (Kind != "class" || !Owner.Parents)
        continue;
      llvm::StringRef Parents(*Owner.Parents);
      while (!Parents.empty()) {
        const auto [Edge, Rest] = Parents.split('|');
        if (Edge == Parent)
          Result.push_back(Class);
        Parents = Rest;
      }
    }
  }
  return Result;
}

std::optional<std::string>
sdkSelectorOutParameterClass(const BinaryImage &Image, llvm::StringRef Selector,
                             unsigned Parameter) {
  const auto *Catalog = catalog(Image.Arch);
  if (!Catalog || Selector.empty())
    return std::nullopt;
  std::optional<std::string> Result;
  bool Seen = false;
  for (const auto &[Provider, Framework] : *Catalog) {
    if (!active(Image, Framework.Modules))
      continue;
    for (const auto &[Key, Owner] : Framework.Owners)
      for (const auto &[MemberKey, Members] : Owner.Members) {
        if (MemberKey.second != Selector)
          continue;
        for (const auto &Member : Members) {
          Seen = true;
          const auto Found = Member.OutParameterClasses.find(Parameter);
          if (!Member.Signature || Found == Member.OutParameterClasses.end() ||
              Found->second.empty() || (Result && *Result != Found->second))
            return std::nullopt;
          Result = Found->second;
        }
      }
  }
  return Seen ? Result : std::nullopt;
}
} // namespace neverd::objc
