#include "neverd/loader/ObjC/ObjCMethods.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace neverd {
namespace {
constexpr size_t MaxRecords = 65536;
constexpr size_t MaxString = 4096;

class RuntimeReader {
  BinaryImage &Img;
  size_t Remaining = MaxRecords;

  void diagnostic(llvm::StringRef Message) {
    if (Img.ObjCMetadataDiagnostics.size() < 64 &&
        std::find(Img.ObjCMetadataDiagnostics.begin(),
                  Img.ObjCMetadataDiagnostics.end(),
                  Message) == Img.ObjCMetadataDiagnostics.end())
      Img.ObjCMetadataDiagnostics.push_back(Message.str());
  }

  const uint8_t *bytes(va_t VA, size_t Size) {
    const auto *Sec = Img.getSectionFor(VA);
    const auto *Seg = Img.getSegmentFor(VA);
    if (!Sec || !Seg || !Sec->isReadable() || !Seg->isReadable() ||
        Size > InvalidVA - VA || Sec->Size > InvalidVA - Sec->VA ||
        Seg->Size > InvalidVA - Seg->VA || Sec->VA < Seg->VA ||
        !rangeInBounds(VA - Sec->VA, Size, Sec->Size) ||
        !rangeInBounds(VA - Seg->VA, Size, Seg->Size) ||
        !rangeInBounds(VA - Sec->VA, Size, Sec->FileSz) ||
        !rangeInBounds(VA - Seg->VA, Size, Seg->FileSz) ||
        Sec->FileOff < Seg->FileOff ||
        Sec->FileOff - Seg->FileOff != Sec->VA - Seg->VA ||
        (Sec->Type & llvm::MachO::SECTION_TYPE) == llvm::MachO::S_ZEROFILL ||
        (Sec->Type & llvm::MachO::SECTION_TYPE) == llvm::MachO::S_GB_ZEROFILL ||
        (Sec->Type & llvm::MachO::SECTION_TYPE) ==
            llvm::MachO::S_THREAD_LOCAL_ZEROFILL)
      return nullptr;
    return Img.readVA(VA, Size);
  }

  std::optional<uint32_t> u32(va_t VA) {
    const auto *P = bytes(VA, 4);
    if (!P)
      return std::nullopt;
    return llvm::support::endian::read32le(P);
  }

  std::optional<va_t> pointer(va_t VA) {
    const auto *P = bytes(VA, 8);
    if (!P)
      return std::nullopt;
    const uint64_t Value = llvm::support::endian::read64le(P);
    if (Value && Img.MachOHasChainedFixups &&
        !Img.MachOResolvedChainedPointerSlots.count(VA)) {
      diagnostic("Objective-C pointer slot has an unresolved chained fixup");
      return std::nullopt;
    }
    return Value;
  }

  std::optional<va_t> relative(va_t VA) {
    auto Offset = u32(VA);
    if (!Offset)
      return std::nullopt;
    const int64_t Signed = static_cast<int32_t>(*Offset);
    if ((Signed < 0 && VA < static_cast<uint64_t>(-Signed)) ||
        (Signed > 0 && VA > InvalidVA - static_cast<uint64_t>(Signed)))
      return std::nullopt;
    return Signed < 0 ? VA - static_cast<uint64_t>(-Signed)
                      : VA + static_cast<uint64_t>(Signed);
  }

  std::optional<std::string> string(va_t VA) {
    std::string Result;
    for (size_t I = 0; I < MaxString; ++I) {
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

  // Deliberately small source-projection grammar. It preserves scalar widths
  // and signedness, but does not invent aggregate, block, or Swift ABI rules.
  static TypeRef type(llvm::StringRef Encoding, size_t &I, unsigned Depth = 0) {
    if (Depth > 16)
      return nullptr;
    while (I < Encoding.size() &&
           llvm::StringRef("rnNoORV").contains(Encoding[I]))
      ++I;
    if (I == Encoding.size())
      return nullptr;
    const char C = Encoding[I++];
    switch (C) {
    case 'v':
      return NdType::makeVoid();
    case 'c':
      return NdType::makeInt(1, true);
    case 'C':
    case 'B':
      return NdType::makeInt(1, false);
    case 's':
      return NdType::makeInt(2, true);
    case 'S':
      return NdType::makeInt(2, false);
    case 'i':
      return NdType::makeInt(4, true);
    case 'I':
      return NdType::makeInt(4, false);
    // Darwin uses q/Q for 64-bit long; legacy l/L encodings remain 32 bits.
    case 'l':
      return NdType::makeInt(4, true);
    case 'L':
      return NdType::makeInt(4, false);
    case 'q':
      return NdType::makeInt(8, true);
    case 'Q':
      return NdType::makeInt(8, false);
    case '#':
    case ':':
      return NdType::makePtr(NdType::makeVoid());
    case '*':
      return NdType::makePtr(NdType::makeInt(1));
    case '@':
      if (I < Encoding.size() && Encoding[I] == '?')
        return nullptr; // Blocks need their own callable ABI model.
      if (I < Encoding.size() && Encoding[I] == '"') {
        const auto End = Encoding.find('"', ++I);
        if (End == llvm::StringRef::npos)
          return nullptr;
        I = End + 1;
      }
      return NdType::makePtr(NdType::makeVoid());
    case '^': {
      auto Pointee = type(Encoding, I, Depth + 1);
      return Pointee ? NdType::makePtr(Pointee) : nullptr;
    }
    default:
      return nullptr;
    }
  }

  static bool skipOffset(llvm::StringRef Encoding, size_t &I) {
    const size_t Begin = I;
    while (I < Encoding.size() && Encoding[I] >= '0' && Encoding[I] <= '9')
      ++I;
    // Offsets are retained as metadata rather than used as physical register
    // assignments. Bound their textual size so malformed integers fail closed.
    return I - Begin <= 10;
  }

  void hint(ObjCMethod &Method) {
    llvm::StringRef Encoding(Method.TypeEncoding);
    size_t I = 0;
    SourceFunctionTypeHint Hint;
    Hint.ReturnType = type(Encoding, I);
    bool Valid = Hint.ReturnType && skipOffset(Encoding, I);
    std::vector<char> Codes;
    while (Valid && I < Encoding.size() && Hint.Parameters.size() < 16) {
      size_t Start = I;
      while (Start < Encoding.size() &&
             llvm::StringRef("rnNoORV").contains(Encoding[Start]))
        ++Start;
      Codes.push_back(Start < Encoding.size() ? Encoding[Start] : '\0');
      auto T = type(Encoding, I);
      if (!T || T->Kind == NdTypeKind::Void || !skipOffset(Encoding, I)) {
        Valid = false;
        break;
      }
      const size_t Index = Hint.Parameters.size();
      Hint.Parameters.push_back({Index == 0 ? "objc_self"
                                 : Index == 1
                                     ? "objc_cmd"
                                     : "arg" + std::to_string(Index - 2),
                                 std::move(T)});
    }
    const size_t Arity =
        std::count(Method.Selector.begin(), Method.Selector.end(), ':');
    if (!Valid || I != Encoding.size() || Hint.Parameters.size() < 2 ||
        (Codes[0] != '@' && Codes[0] != '#') || Codes[1] != ':' ||
        Hint.Parameters.size() != Arity + 2) {
      Method.Status = "unsupported_encoding";
      Method.Diagnostics.push_back(
          "Unsupported or inconsistent Objective-C type encoding");
      return;
    }
    const size_t RegisterCount = Img.Arch == Arch::AArch64 ? 8 : 6;
    if (Hint.Parameters.size() > RegisterCount) {
      Method.Status = "unsupported_abi";
      Method.Diagnostics.push_back(
          "Stack-passed method parameters are not projected");
      return;
    }
    Method.TypeHint = std::move(Hint);
    Method.Status = "supported";
    Method.Diagnostics.push_back("Runtime type hint is unverified; fixed "
                                 "parameters only, variadic tail unknown");
  }

  void methods(const ObjCClass &Class, va_t RO, bool ClassMethod) {
    auto List = pointer(RO + 32);
    if (!List) {
      diagnostic("Objective-C method-list pointer is unavailable");
      return;
    }
    if (!*List)
      return;
    auto Flags = u32(*List);
    auto Count = *List <= InvalidVA - 4 ? u32(*List + 4) : std::nullopt;
    if (!Flags || !Count) {
      diagnostic("Truncated Objective-C method list");
      return;
    }
    const bool Small = (*Flags & 0x80000000U) != 0;
    const bool DirectSelectors = (*Flags & 0x40000000U) != 0;
    const uint32_t EntrySize = (*Flags & 0xffffU) & ~3U;
    if (EntrySize < (Small ? 12U : 24U) || EntrySize > 4096 ||
        *Count > Remaining ||
        !bytes(*List, 8ULL + uint64_t(*Count) * EntrySize)) {
      diagnostic("Invalid or excessive Objective-C method list");
      return;
    }
    Remaining -= *Count;
    for (uint32_t N = 0; N < *Count; ++N) {
      const va_t Entry = *List + 8 + uint64_t(N) * EntrySize;
      auto Selector = Small ? relative(Entry) : pointer(Entry);
      if (Small && !DirectSelectors && Selector)
        Selector = pointer(*Selector);
      auto Types = Small ? relative(Entry + 4) : pointer(Entry + 8);
      auto IMP = Small ? relative(Entry + 8) : pointer(Entry + 16);
      ObjCMethod Method;
      Method.MetadataAddress = Entry;
      Method.ClassAddress = Class.Address;
      Method.ClassName = Class.Name;
      Method.IsClassMethod = ClassMethod;
      if (Selector)
        Method.Selector = string(*Selector).value_or("");
      if (Types)
        Method.TypeEncoding = string(*Types).value_or("");
      Method.Implementation = IMP.value_or(0);
      const size_t Align = Img.Arch == Arch::AArch64 ? 4 : 1;
      if (!IMP || !*IMP || *IMP % Align || !Img.isCodeAddress(*IMP) ||
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
        if (!Img.hasFunctionSymbolAt(*IMP))
          Img.addSymbol("objc_imp_" + llvm::utohexstr(*IMP), *IMP, 0, true);
      }
      Img.ObjCMethods.push_back(std::move(Method));
    }
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
    auto Meta = pointer(VA);
    if (Meta && *Meta) {
      auto MetaRO = classRO(*Meta);
      if (MetaRO && (u32(*MetaRO).value_or(0) & 1))
        methods(Class, *MetaRO, true);
      else
        diagnostic("Objective-C metaclass is unavailable");
    }
  }

public:
  explicit RuntimeReader(BinaryImage &Image) : Img(Image) {}

  void run() {
    std::set<va_t> Classes;
    for (const Section &Sec : Img.Sections) {
      if (Sec.Name == "__objc_catlist" || Sec.Name == "__objc_nlcatlist") {
        if (Sec.Size)
          diagnostic("Objective-C category methods are not yet projected");
        continue;
      }
      if (Sec.Name != "__objc_classlist" && Sec.Name != "__objc_nlclslist")
        continue;
      if (Sec.Size % 8 || Sec.Size / 8 > MaxRecords ||
          Img.getSectionFor(Sec.VA) != &Sec || !bytes(Sec.VA, Sec.Size)) {
        diagnostic("Invalid or non-file-backed Objective-C class list");
        continue;
      }
      for (size_t I = 0; I < Sec.Size / 8; ++I) {
        auto Class = pointer(Sec.VA + I * 8);
        if (Class && *Class && Classes.insert(*Class).second) {
          if (Classes.size() > MaxRecords) {
            diagnostic("Objective-C class count exceeds the parsing budget");
            break;
          }
          readClass(*Class);
        }
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
