#include "neverd/loader/ObjC/ObjCMethods.h"

#include "ObjCMethodLists.h"
#include "ObjCProperties.h"
#include "ObjCProtocols.h"
#include "ObjCRuntimeData.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/MachO/DyldSharedCache.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"
#include "neverd/loader/SymbolSpelling.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

namespace neverd {
namespace {

#define NEVERD_OBJC_RELATIVE_LIST_TEXT(Name, Text)                             \
  inline constexpr char Name[] = Text;
#include "ObjCRelativeLists.def"

/// The name a method implementation with no symbol takes: clang's
/// `-[Class(Category) selector:]` (ObjCMethodSpellings.def), or
/// `objc_imp_<address>` when the metadata spells a class, category or
/// selector that is not an Objective-C name.
std::string implementationName(const ObjCClass &Class, llvm::StringRef Category,
                               bool ClassMethod, llvm::StringRef Selector,
                               va_t IMP) {
  llvm::StringRef Marker;
#define NEVERD_OBJC_METHOD_KIND(Spelling, CPrefix, IsClassMethod)              \
  if (IsClassMethod == ClassMethod)                                            \
    Marker = Spelling;
#include "neverd/loader/ObjCMethodSpellings.def"
  std::string Name = (Marker + "[" + Class.Name).str();
  if (!Category.empty())
    Name += ("(" + Category + ")").str();
  Name += (" " + Selector + "]").str();
  if (symbolScheme(Name) == SymbolScheme::ObjCMethod)
    return Name;
  return "objc_imp_" + llvm::utohexstr(IMP);
}

class RuntimeReader {
  BinaryImage &Img;
  size_t Remaining = ::neverd::limits::loader::kObjCMethodsMaxRecords;

  void diagnostic(llvm::StringRef Message) {
    if (Img.ObjCMetadataDiagnostics.size() <
            ::neverd::limits::loader::
                kObjCMethodsDiagnosticImgObjCMetadataDiagnosticsSizeLimit &&
        std::find(Img.ObjCMetadataDiagnostics.begin(),
                  Img.ObjCMetadataDiagnostics.end(),
                  Message) == Img.ObjCMetadataDiagnostics.end())
      Img.ObjCMetadataDiagnostics.push_back(Message.str());
  }

  const uint8_t *bytes(va_t VA, size_t Size) {
    const objc::RuntimeData Data(Img, objc::PointerView::MetadataTarget);
    return Data.bytes(VA, Size);
  }

  std::optional<uint32_t> u32(va_t VA) {
    const auto *P = bytes(VA, 4);
    if (!P)
      return std::nullopt;
    return llvm::support::endian::read32le(P);
  }

  std::optional<va_t> pointer(va_t VA) {
    const objc::RuntimeData Data(Img, objc::PointerView::MetadataTarget);
    const auto Value = Data.pointer(VA);
    if (!Value)
      diagnostic(macho_loader::arm64e::kObjCMetadataPointerUnavailable);
    return Value;
  }

  std::optional<std::string> string(va_t VA) {
    std::string Result;
    for (size_t I = 0; I < ::neverd::limits::loader::kObjCMethodsMaxString;
         ++I) {
      if (VA > InvalidVA - I)
        return std::nullopt;
      const auto *P = bytes(VA + I, 1);
      if (!P)
        return std::nullopt;
      if (!*P)
        return Result.empty() ? std::nullopt
                              : std::optional<std::string>(Result);
      if (*P < 0x20 || *P > 0x7e)
        return std::nullopt;
      Result.push_back(static_cast<char>(*P));
    }
    return std::nullopt;
  }

  std::optional<va_t> classRO(va_t Class) {
    if (!bytes(Class, 40))
      return std::nullopt;
    auto Bits = pointer(Class + 32);
    if (!Bits || !*Bits)
      return std::nullopt;
    const va_t RO = *Bits & ~uint64_t(7);
    return bytes(RO, 40) ? std::optional<va_t>(RO) : std::nullopt;
  }

  std::optional<std::string> className(va_t Class) {
    auto RO = classRO(Class);
    if (!RO)
      return std::nullopt;
    auto Name = pointer(*RO + 24);
    return Name ? string(*Name) : std::nullopt;
  }

  void hint(ObjCMethod &Method) {
    auto Hint = parseObjCMethodEncoding(Method.Selector, Method.TypeEncoding);
    if (!Hint) {
      Method.Status = "unsupported_encoding";
      Method.Diagnostics.push_back(
          "Unsupported or inconsistent Objective-C type encoding");
      return;
    }
    std::string ABIDiagnostic;
    if (!assignDarwinObjCSourceABI(*Hint, Img.Arch, ABIDiagnostic)) {
      Method.Status = "unsupported_abi";
      Method.Diagnostics.push_back(std::move(ABIDiagnostic));
      return;
    }
    Method.TypeHint = std::move(Hint);
    Method.Status = kSupportedMethod;
    Method.Diagnostics.push_back("Runtime type hint is unverified; fixed "
                                 "parameters only, variadic tail unknown");
  }

  void methodList(const ObjCClass &Class, va_t List, bool ClassMethod,
                  llvm::StringRef Category = {}) {
    if (!List)
      return;
    std::string Diagnostic;
    auto Records = objc::readMethodList(Img, List, Remaining, Diagnostic);
    if (!Records) {
      diagnostic(Diagnostic);
      return;
    }
    for (auto &Record : *Records) {
      const auto IMP = Record.Implementation;
      ObjCMethod Method;
      Method.MetadataAddress = Record.Address;
      Method.CacheOrigin = Record.CacheOrigin;
      Method.ClassAddress = Class.Address;
      Method.ClassName = Class.Name;
      Method.IsClassMethod = ClassMethod;
      Method.Selector = std::move(Record.Selector);
      Method.TypeEncoding = std::move(Record.TypeEncoding);
      Method.Implementation = IMP.value_or(0);
      const size_t Align = Img.Arch == Arch::AArch64 ? 4 : 1;
      const auto *CacheMapping = IMP && Img.MachOSharedCache
                                     ? Img.MachOSharedCache->mappingFor(*IMP)
                                     : nullptr;
      if (IMP && *IMP && !(*IMP % Align) && CacheMapping &&
          (CacheMapping->InitialProtection & llvm::MachO::VM_PROT_EXECUTE) &&
          !Img.isCodeAddress(*IMP) && bytes(*IMP, Align)) {
        Method.Status = kExternalImplementation;
        Method.Diagnostics.push_back(kExternalImplementationDiagnostic);
      } else if (!IMP || !*IMP || *IMP % Align || !Img.isCodeAddress(*IMP) ||
                 !bytes(*IMP, Align)) {
        Method.Status = "invalid_implementation";
        Method.Diagnostics.push_back(
            "Method IMP is not resolved file-backed executable code");
      } else if (Method.Selector.empty() || Method.TypeEncoding.empty()) {
        Method.Status = "invalid_metadata";
        Method.Diagnostics.push_back(
            "Method selector or type encoding is unavailable");
      } else {
        hint(Method);
        // LC_FUNCTION_STARTS proves an entry, but its synthesized name must
        // not hide a method identity recovered from validated metadata.
        const Symbol *Name = Img.functionNameSymbolAt(*IMP);
        if (Name && Name->IsFunc && Name->Origin == NameOrigin::Synthesized) {
          Symbol &Existing = Img.Symbols[Name - Img.Symbols.data()];
          Existing.Name = implementationName(Class, Category, ClassMethod,
                                             Method.Selector, *IMP);
          Existing.Origin = NameOrigin::Stated;
          Existing.IsBoundaryGuess = false;
        } else if (!Img.hasFunctionSymbolAt(*IMP)) {
          Img.addSymbol(implementationName(Class, Category, ClassMethod,
                                           Method.Selector, *IMP),
                        *IMP, 0, true);
        }
      }
      Img.ObjCMethods.push_back(std::move(Method));
    }
  }

  void methods(const ObjCClass &Class, va_t RO, bool ClassMethod) {
    auto List = pointer(RO + 32);
    if (!List) {
      diagnostic("Objective-C method-list pointer is unavailable");
      return;
    }
    methodList(Class, *List, ClassMethod);
  }

  void properties(const ObjCClass &Class, va_t RO, bool ClassProperty) {
    ObjCProperty Owner;
    Owner.OwnerAddress = Class.Address;
    Owner.OwnerName = Owner.ClassName = Class.Name;
    Owner.IsClassProperty = ClassProperty;
    objc::readPropertyList(Img, RO + 64, Owner, Remaining);
  }

  void readClass(va_t VA) {
    auto RO = classRO(VA);
    auto Name = className(VA);
    if (!RO || !Name || (u32(*RO).value_or(0) & 1)) {
      diagnostic("Objective-C class or class_ro_t is unavailable");
      return;
    }
    ObjCClass Class;
    Class.Address = VA;
    Class.Name = *Name;
    const bool RootFlag = (u32(*RO).value_or(0) & 2) != 0;
    auto Super = pointer(VA + 8);
    if (Super) {
      Class.SuperclassAddress = *Super;
      if (*Super)
        Class.SuperclassName = className(*Super).value_or("");
    }
    auto Bound = Img.DyldBindSlots.find(VA + 8);
    if (Bound != Img.DyldBindSlots.end() && Bound->second.Addend == 0) {
      llvm::StringRef Name(Bound->second.Name);
      if (Name.consume_front("_OBJC_CLASS_$_"))
        Class.SuperclassName = Name.str();
    }
    Class.RootClass =
        RootFlag && Super && !*Super && Bound == Img.DyldBindSlots.end();
    Class.InheritanceStatus = !Class.SuperclassName.empty() ? "resolved"
                              : Class.RootClass && Super && !*Super
                                  ? "root"
                                  : "unresolved";
    Img.ObjCClasses.push_back(Class);
    methods(Class, *RO, false);
    properties(Class, *RO, false);
    auto Meta = pointer(VA);
    if (Meta && *Meta) {
      auto MetaRO = classRO(*Meta);
      if (MetaRO && (u32(*MetaRO).value_or(0) & 1)) {
        methods(Class, *MetaRO, true);
        properties(Class, *MetaRO, true);
      } else
        diagnostic("Objective-C metaclass is unavailable");
    }
  }

  void readCategory(va_t VA, const ImportStorageSlotCollection &Imports,
                    std::set<va_t> &Classes) {
    const objc::RuntimeData Data(Img, objc::PointerView::MetadataTarget);
    // The non-fragile category ABI has six common pointer fields. The seventh
    // (class properties) and newer compiler size fields are optional on disk;
    // do not infer their presence from sizeof a current runtime structure.
    constexpr uint64_t CommonPrefixBytes = 6 * 8;
    if (!Data.bytes(VA, CommonPrefixBytes)) {
      diagnostic("Truncated or non-file-backed Objective-C category record");
      return;
    }
    const auto NamePointer = Data.pointer(VA);
    const auto Name = NamePointer ? Data.string(*NamePointer) : std::nullopt;
    if (!Name) {
      diagnostic("Objective-C category name is unavailable");
      return;
    }
    ObjCClass Owner;
    const va_t ClassSlot = VA + 8;
    if (Imports.Conflicts.count(ClassSlot)) {
      diagnostic("Objective-C category class has conflicting import bindings");
      return;
    }
    if (auto Bound = Imports.Slots.find(ClassSlot);
        Bound != Imports.Slots.end()) {
      llvm::StringRef ClassName(Bound->second.Name);
      if (Bound->second.Addend != 0 ||
          !ClassName.consume_front("_OBJC_CLASS_$_") || ClassName.empty()) {
        diagnostic("Objective-C category target is not an exact class binding");
        return;
      }
      Owner.Name = ClassName.str();
      // An imported class is an external dependency. Preserve its name on the
      // category methods without fabricating a local class or instance layout.
    } else {
      const auto ClassPointer = Data.pointer(ClassSlot);
      const auto RO = ClassPointer ? Data.classRO(*ClassPointer) : std::nullopt;
      const auto Flags = RO ? Data.u32(*RO) : std::nullopt;
      const auto ClassName =
          ClassPointer ? Data.className(*ClassPointer) : std::nullopt;
      if (!ClassPointer || !*ClassPointer || !Flags || (*Flags & 1) ||
          !ClassName) {
        diagnostic(
            "Objective-C category target class is unresolved or a metaclass");
        return;
      }
      if (Classes.insert(*ClassPointer).second) {
        if (Classes.size() > ::neverd::limits::loader::kObjCMethodsMaxRecords) {
          diagnostic("Objective-C class count exceeds the parsing budget");
          return;
        }
        readClass(*ClassPointer);
      }
      auto Existing =
          std::find_if(Img.ObjCClasses.begin(), Img.ObjCClasses.end(),
                       [&](const ObjCClass &Class) {
                         return Class.Address == *ClassPointer;
                       });
      if (Existing == Img.ObjCClasses.end()) {
        diagnostic("Objective-C category target class metadata is incomplete");
        return;
      }
      Owner = *Existing;
    }
    const size_t FirstMethod = Img.ObjCMethods.size();
    for (const auto &[Offset, IsClassMethod] :
         {std::pair<uint64_t, bool>{16, false}, {24, true}}) {
      auto List = Data.pointer(VA + Offset);
      if (!List) {
        diagnostic("Objective-C category method-list pointer is unavailable");
        continue;
      }
      methodList(Owner, *List, IsClassMethod, *Name);
    }
    for (size_t I = FirstMethod; I < Img.ObjCMethods.size(); ++I) {
      Img.ObjCMethods[I].CategoryName = *Name;
      Img.ObjCMethods[I].CategoryAddress = VA;
    }
    ObjCProperty PropertyOwner;
    PropertyOwner.Owner = ObjCProperty::OwnerKind::Category;
    PropertyOwner.OwnerAddress = VA;
    PropertyOwner.OwnerName = *Name;
    PropertyOwner.ClassName = Owner.Name;
    objc::readPropertyList(Img, VA + 40, PropertyOwner, Remaining);
    if (objc::hasCategoryClassProperties(Img)) {
      PropertyOwner.IsClassProperty = true;
      objc::readPropertyList(Img, VA + 48, PropertyOwner, Remaining);
    }
  }

public:
  explicit RuntimeReader(BinaryImage &Image) : Img(Image) {}

  void run() {
    std::set<va_t> Classes;
    for (const Section &Sec : Img.Sections) {
      if (Sec.Name != "__objc_classlist" && Sec.Name != "__objc_nlclslist")
        continue;
      if (Sec.Size % 8 ||
          Sec.Size / 8 > ::neverd::limits::loader::kObjCMethodsMaxRecords ||
          Img.getSectionFor(Sec.VA) != &Sec || !bytes(Sec.VA, Sec.Size)) {
        diagnostic("Invalid or non-file-backed Objective-C class list");
        continue;
      }
      for (size_t I = 0; I < Sec.Size / 8; ++I) {
        auto Class = pointer(Sec.VA + I * 8);
        if (Class && *Class && Classes.insert(*Class).second) {
          if (Classes.size() >
              ::neverd::limits::loader::kObjCMethodsMaxRecords) {
            diagnostic("Objective-C class count exceeds the parsing budget");
            break;
          }
          readClass(*Class);
        }
      }
    }
    // Categories may precede their classes in section order, and +load entries
    // appear in both lists. Resolve classes first and visit each category once.
    const auto Imports = Img.collectImportStorageSlots();
    const objc::RuntimeData Data(Img, objc::PointerView::MetadataTarget);
    std::set<va_t> Categories;
    for (const Section &Sec : Img.Sections) {
      if (Sec.Name != "__objc_catlist" && Sec.Name != "__objc_nlcatlist")
        continue;
      if (Sec.Size % 8 ||
          Sec.Size / 8 > ::neverd::limits::loader::kObjCMethodsMaxRecords ||
          Img.getSectionFor(Sec.VA) != &Sec || !Data.bytes(Sec.VA, Sec.Size)) {
        diagnostic("Invalid or non-file-backed Objective-C category list");
        continue;
      }
      for (size_t I = 0; I < Sec.Size / 8; ++I) {
        auto Category = Data.pointer(Sec.VA + I * 8);
        if (!Category || !*Category) {
          diagnostic("Objective-C category address is unavailable");
          continue;
        }
        if (!Categories.insert(*Category).second)
          continue;
        if (Categories.size() >
            ::neverd::limits::loader::kObjCMethodsMaxRecords) {
          diagnostic("Objective-C category count exceeds the parsing budget");
          break;
        }
        readCategory(*Category, Imports, Classes);
      }
    }
  }
};

bool sameType(const TypeRef &A, const TypeRef &B) {
  return A && B && A->Kind == B->Kind && A->Size == B->Size &&
         A->IsSigned == B->IsSigned &&
         (A->Kind != NdTypeKind::Ptr || sameType(A->Pointee, B->Pointee));
}

bool sameHint(const ObjCMethod &A, const ObjCMethod &B) {
  if (!A.TypeHint || !B.TypeHint ||
      !sameType(A.TypeHint->ReturnType, B.TypeHint->ReturnType) ||
      A.TypeHint->Parameters.size() != B.TypeHint->Parameters.size())
    return false;
  for (size_t I = 0; I < A.TypeHint->Parameters.size(); ++I)
    if (!sameType(A.TypeHint->Parameters[I].Type,
                  B.TypeHint->Parameters[I].Type))
      return false;
  return true;
}
} // namespace

void parseObjCMethods(BinaryImage &Img) {
  Img.ObjCClasses.clear();
  Img.ObjCMethods.clear();
  Img.ObjCProtocols.clear();
  Img.ObjCProperties.clear();
  Img.ObjCMetadataDiagnostics.clear();
  if (!Img.isMachO())
    return;
  if (Img.IsRelocatable || Img.Bits != Bitness::Bits64 ||
      (Img.Arch != Arch::AArch64 && Img.Arch != Arch::X64)) {
    Img.ObjCMetadataDiagnostics.push_back(
        "Objective-C source types require a linked arm64 or x86_64 image");
    return;
  }
  if (Img.MachOChainedFixupsAmbiguous) {
    Img.ObjCMetadataDiagnostics.push_back(
        "Duplicate chained-fixup commands prevent Objective-C metadata "
        "decoding");
    return;
  }
  RuntimeReader(Img).run();
  objc::readProtocolDeclarations(Img);
  // Class and category lists do not establish a unique runtime override order
  // for colliding declarations. Keep every record and its category identity,
  // but never emit several candidate bodies as one selected method.
  std::map<std::tuple<std::string, bool, std::string>,
           std::vector<ObjCMethod *>>
      BySelector;
  for (auto &Method : Img.ObjCMethods)
    if (!Method.Selector.empty())
      BySelector[{Method.ClassName, Method.IsClassMethod, Method.Selector}]
          .push_back(&Method);
  for (auto &[Identity, Methods] : BySelector)
    if (Methods.size() > 1)
      for (auto *Method : Methods) {
        // Dispatch order does not erase a validated declaration's ABI. Calls
        // still require every matching declaration to agree; body selection
        // separately requires supported status and must not pick an override.
        if (Method->Status == kSupportedMethod)
          Method->Status = "ambiguous_dispatch";
        Method->Diagnostics.push_back(
            "Duplicate class/category selector declarations have no unique "
            "recovered dispatch order");
      }
  std::map<va_t, std::vector<ObjCMethod *>> ByIMP;
  for (auto &Method : Img.ObjCMethods)
    if (Method.Implementation)
      ByIMP[Method.Implementation].push_back(&Method);
  for (auto &[IMP, Methods] : ByIMP) {
    if (Methods.size() < 2)
      continue;
    bool Conflict = false;
    for (size_t I = 1; I < Methods.size(); ++I)
      Conflict |= !sameHint(*Methods[0], *Methods[I]);
    if (Conflict)
      for (auto *Method : Methods) {
        Method->TypeHint.reset();
        Method->Status = "conflicting_encoding";
        Method->Diagnostics.push_back(
            "Methods sharing this IMP have incompatible or incomplete "
            "declarations");
      }
  }
}
} // namespace neverd
