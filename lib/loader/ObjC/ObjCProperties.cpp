#include "ObjCProperties.h"

#include "ObjCRelativeLists.h"
#include "ObjCRuntimeData.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <map>

namespace neverd::objc {
namespace {
void diagnostic(BinaryImage &Image, llvm::StringRef Message) {
  auto &Messages = Image.ObjCMetadataDiagnostics;
  if (Messages.size() < ::neverd::limits::loader::
                            kObjCPropertiesDiagnosticMessagesSizeLimit &&
      std::find(Messages.begin(), Messages.end(), Message) == Messages.end())
    Messages.push_back(Message.str());
}

bool identifier(llvm::StringRef Name) {
  if (Name.empty() || !(llvm::isAlpha(Name.front()) || Name.front() == '_' ||
                        Name.front() == '$'))
    return false;
  return std::all_of(Name.begin(), Name.end(), [](char C) {
    return llvm::isAlnum(C) || C == '_' || C == '$';
  });
}

// Commas inside quoted object types or compound pointee encodings do not
// delimit attributes. Reject unbalanced or duplicate fields before deriving
// any accessor contract.
std::optional<std::map<char, std::string>> attributes(llvm::StringRef Text) {
  std::map<char, std::string> Result;
  std::string Closers;
  bool Quoted = false;
  size_t Start = 0;
  for (size_t I = 0; I <= Text.size(); ++I) {
    const char C = I == Text.size() ? ',' : Text[I];
    if (C == '\\')
      return std::nullopt;
    if (C == '"')
      Quoted = !Quoted;
    if (!Quoted) {
      const auto Open = llvm::StringRef("{[(").find(C);
      if (Open != llvm::StringRef::npos)
        Closers.push_back("}])"[Open]);
      else if (llvm::StringRef("}])").contains(C)) {
        if (Closers.empty() || Closers.back() != C)
          return std::nullopt;
        Closers.pop_back();
      }
      if (C == ',' && Closers.empty()) {
        if (Start == I ||
            !Result.emplace(Text[Start], Text.slice(Start + 1, I).str()).second)
          return std::nullopt;
        Start = I + 1;
      }
    }
  }
  if (Quoted || !Closers.empty() || Start != Text.size() + 1)
    return std::nullopt;
  return Result;
}

void accessorHints(ObjCProperty &Property, Arch Architecture) {
  if (!identifier(Property.Name))
    return;
  Property.Getter = Property.Name;
  Property.Setter = "set" + Property.Name + ":";
  Property.Setter[3] = llvm::toUpper(Property.Setter[3]);
  const auto Fields = attributes(Property.Attributes);
  if (!Fields)
    return;
  bool Valid = Fields->count('T') != 0;
  unsigned Ownership = 0;
  for (const auto &[Kind, Value] : *Fields) {
    switch (Kind) {
    case 'T':
      Property.TypeEncoding = Value;
      Valid &= !Value.empty();
      break;
    case 'G':
      Property.Getter = Value;
      Valid &= identifier(Value);
      break;
    case 'S': {
      Property.Setter = Value;
      llvm::StringRef Selector(Value);
      Valid &= Selector.consume_back(":") && identifier(Selector);
      break;
    }
    case 'R':
      Property.ReadOnly = true;
      Valid &= Value.empty();
      break;
    case '?':
      Property.IsOptional = true;
      Valid &= Value.empty();
      break;
    case 'C':
    case '&':
    case 'W':
      ++Ownership;
      [[fallthrough]];
    case 'N':
    case 'D':
    case 'P':
      Valid &= Value.empty();
      break;
    case 'V':
      Valid &= identifier(Value);
      break;
    default:
      Valid = false;
      break;
    }
  }
  Valid &= Ownership <= 1 && !(Property.ReadOnly && Fields->count('S'));
  if (Property.ReadOnly && !Fields->count('S'))
    Property.Setter.clear();
  if (!Valid)
    return;
  size_t Offset = 0;
  auto Type = parseObjCSourceType(Property.TypeEncoding, Offset);
  if (!Type || Type->Kind == NdTypeKind::Void ||
      Offset != Property.TypeEncoding.size()) {
    Property.Status = "unsupported_encoding";
    return;
  }
  auto Signature = [&](const std::string &Selector,
                       const std::string &Encoding) {
    auto Hint = parseObjCMethodEncoding(Selector, Encoding);
    std::string Diagnostic;
    if (Hint && !assignDarwinObjCSourceABI(*Hint, Architecture, Diagnostic))
      Hint.reset();
    return Hint;
  };
  Property.GetterTypeHint =
      Signature(Property.Getter, Property.TypeEncoding + "@:");
  if (!Property.ReadOnly)
    Property.SetterTypeHint =
        Signature(Property.Setter, "v@:" + Property.TypeEncoding);
  Property.Status =
      Property.GetterTypeHint && (Property.ReadOnly || Property.SetterTypeHint)
          ? "supported"
          : "unsupported_abi";
}
} // namespace

namespace {
bool readOnePropertyList(BinaryImage &Image, va_t Address,
                         const ObjCProperty &Owner, size_t &Remaining) {
  const RuntimeData Data(Image, PointerView::MetadataTarget);
  if (Address % 8 || !Data.bytes(Address, 8)) {
    diagnostic(Image,
               "Invalid or unsupported Objective-C property-list layout");
    return false;
  }
  const auto Stride = Data.u32(Address);
  const auto Count = Data.u32(Address + 4);
  if (Stride && !*Stride && Count && !*Count)
    return true;
  // Each leaf property record contains two absolute pointers. Relative list
  // arrays have already been resolved by the common reader.
  if (!Stride || *Stride != 16 || !Count || *Count > Remaining ||
      !Data.bytes(Address, 8 + uint64_t(*Count) * 16)) {
    diagnostic(Image,
               "Invalid or unsupported Objective-C property-list layout");
    return false;
  }
  Remaining -= *Count;
  bool Valid = true;
  for (uint32_t I = 0; I != *Count; ++I) {
    auto Property = Owner;
    Property.MetadataAddress = Address + 8 + uint64_t(I) * 16;
    const auto Name = Data.localPointer(Property.MetadataAddress);
    const auto Attributes = Data.localPointer(Property.MetadataAddress + 8);
    Property.Name = Name ? Data.string(*Name).value_or("") : "";
    Property.Attributes =
        Attributes ? Data.string(*Attributes).value_or("") : "";
    accessorHints(Property, Image.Arch);
    if (Property.Status == "invalid_metadata") {
      Valid = false;
      diagnostic(Image, "Invalid Objective-C property name or attributes");
    }
    Image.ObjCProperties.push_back(std::move(Property));
  }
  return Valid;
}
} // namespace

bool readPropertyList(BinaryImage &Image, va_t Slot, const ObjCProperty &Owner,
                      size_t &Remaining) {
  const RuntimeData Data(Image, PointerView::MetadataTarget);
  const auto List = Data.localPointer(Slot);
  if (!List) {
    diagnostic(Image, "Objective-C property-list pointer is unavailable");
    return false;
  }
  auto References = readListReferences(Image, *List, Remaining);
  if (!References) {
    diagnostic(Image, llvm::toString(References.takeError()));
    return false;
  }
  bool Valid = true;
  for (const auto &Reference : *References) {
    auto DeclarationOwner = Owner;
    DeclarationOwner.CacheOrigin = Reference.CacheOrigin;
    Valid = readOnePropertyList(Image, Reference.Address, DeclarationOwner,
                                Remaining) &&
            Valid;
  }
  return Valid;
}

bool hasCategoryClassProperties(BinaryImage &Image) {
  const RuntimeData Data(Image);
  std::optional<uint32_t> Flags;
  for (const auto &Section : Image.Sections)
    if (Section.Name == "__objc_imageinfo") {
      const auto Version = Data.u32(Section.VA);
      const auto Value = Data.u32(Section.VA + 4);
      if (Flags || Section.Size != 8 || !Version || *Version || !Value) {
        diagnostic(Image,
                   "Invalid Objective-C image-info category property layout");
        return false;
      }
      Flags = Value;
    }
  return Flags && (*Flags & (1U << 6));
}
} // namespace neverd::objc
