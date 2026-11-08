//===- ELFProgramLinking.cpp - Original dynamic linker inputs -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/loader/ELF/ELFProgramLinking.h"

#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/Limits.h"

#include "llvm/Object/ELF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/LEB128.h"

#include <cstring>
#include <map>
#include <set>

namespace neverd {
namespace {
using ELFT = llvm::object::ELF64LE;
using namespace llvm::ELF;
llvm::Error failure(llvm::Twine Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "ELF dynamic linking: " + Message);
}
class Reader {
  const BinaryImage &Image;

public:
  explicit Reader(const BinaryImage &Image) : Image(Image) {}
  llvm::Expected<llvm::ArrayRef<uint8_t>> bytes(uint64_t VA, uint64_t Size) {
    if (VA > UINT64_MAX - Size)
      return failure("table address overflows");
    std::optional<uint64_t> Offset;
    for (const auto &P : Image.ELFMetadata->ProgramHeaders) {
      if (P.Type != PT_LOAD || VA < P.VirtualAddress ||
          VA - P.VirtualAddress > P.FileSize ||
          Size > P.FileSize - (VA - P.VirtualAddress))
        continue;
      if (P.FileOffset > Image.Raw.size() ||
          P.FileSize > Image.Raw.size() - P.FileOffset)
        return failure("truncated PT_LOAD");
      uint64_t Candidate = P.FileOffset + VA - P.VirtualAddress;
      if (Offset && *Offset != Candidate)
        return failure("ambiguous table mapping");
      Offset = Candidate;
    }
    if (!Offset)
      return failure("table is not backed by PT_LOAD file bytes");
    return llvm::ArrayRef<uint8_t>(Image.Raw.data() + *Offset, Size);
  }
  template <class T> llvm::Expected<T> record(uint64_t VA) {
    auto B = bytes(VA, sizeof(T));
    if (!B)
      return B.takeError();
    T V;
    std::memcpy(&V, B->data(), sizeof(V));
    return V;
  }
};
} // namespace

llvm::Expected<ELFProgramLinking>
readELFProgramLinking(const BinaryImage &Image) {
  if (!Image.isELF() || !Image.ELFMetadata || Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::X64 && Image.Arch != Arch::AArch64))
    return failure("requires ELF64LE program metadata");
  auto Dynamic = readELFProgramDynamicTable(Image);
  if (!Dynamic)
    return Dynamic.takeError();
  Reader R(Image);
  for (const auto &P : Image.ELFMetadata->ProgramHeaders) {
    if (P.Type != PT_DYNAMIC)
      continue;
    auto Mapped = R.bytes(P.VirtualAddress, P.FileSize);
    if (!Mapped)
      return Mapped.takeError();
    if (Mapped->data() != Image.Raw.data() + P.FileOffset)
      return failure("PT_DYNAMIC file and virtual mappings disagree");
  }
  ELFProgramLinking Out;
  Out.Dynamic = std::move(*Dynamic);
  if (Out.Dynamic.size() >
      ::neverd::limits::loader::kELFProgramLinkingMaxRecords)
    return failure("too many dynamic records");
  std::map<int64_t, uint64_t> Tags;
  for (const auto &D : Out.Dynamic) {
    if (D.Tag == DT_NEEDED)
      continue;
    if (!Tags.emplace(D.Tag, D.Value).second)
      return failure("duplicate dynamic tag");
  }
  auto Get = [&](int64_t Tag) {
    auto I = Tags.find(Tag);
    return I == Tags.end() ? 0 : I->second;
  };
  if (Get(DT_RELSZ) || Get(DT_ANDROID_RELSZ))
    return failure("REL dynamic relocations are unsupported");
  std::set<uint64_t> Slots;
  auto Add = [&](uint64_t Address, int64_t Addend, uint32_t Type,
                 uint32_t Symbol, bool Explicit) -> llvm::Error {
    // NONE may legitimately have a zero offset and repeats without effects.
    if (Type && !Slots.insert(Address).second)
      return failure("duplicate relocation destination");
    if (Out.Relocations.size() >=
        ::neverd::limits::loader::kELFProgramLinkingMaxRecords)
      return failure("too many relocations");
    Out.Relocations.push_back({Address, Addend, Type, Symbol, Explicit});
    return llvm::Error::success();
  };
  auto Rela = [&](int64_t AddrTag, int64_t SizeTag) -> llvm::Error {
    uint64_t Size = Get(SizeTag), Address = Get(AddrTag);
    if (!Size)
      return llvm::Error::success();
    if (!Address || Size % sizeof(ELFT::Rela) ||
        Size / sizeof(ELFT::Rela) >
            ::neverd::limits::loader::kELFProgramLinkingMaxRecords)
      return failure("invalid RELA table extent");
    auto B = R.bytes(Address, Size);
    if (!B)
      return B.takeError();
    for (uint64_t I = 0; I < Size; I += sizeof(ELFT::Rela)) {
      ELFT::Rela V;
      std::memcpy(&V, B->data() + I, sizeof(V));
      if (auto E = Add(V.r_offset, V.r_addend, V.getType(false),
                       V.getSymbol(false), true))
        return E;
    }
    return llvm::Error::success();
  };
  if ((Get(DT_RELASZ) && Get(DT_RELAENT) != sizeof(ELFT::Rela)) ||
      (Get(DT_PLTRELSZ) && Get(DT_PLTREL) != DT_RELA))
    return failure("invalid relocation entry format");
  if (auto E = Rela(DT_RELA, DT_RELASZ))
    return std::move(E);
  if (auto E = Rela(DT_JMPREL, DT_PLTRELSZ))
    return std::move(E);
  if (uint64_t Size = Get(DT_ANDROID_RELASZ)) {
    auto B = R.bytes(Get(DT_ANDROID_RELA), Size);
    if (!B)
      return B.takeError();
    if (Size < 6 || std::memcmp(B->data(), "APS2", 4))
      return failure("invalid Android packed relocation header");
    const char *Error = nullptr;
    int64_t Count =
        llvm::decodeSLEB128(B->data() + 4, nullptr, B->end(), &Error);
    if (Error || Count < 0 ||
        uint64_t(Count) >
            ::neverd::limits::loader::kELFProgramLinkingMaxRecords)
      return failure("invalid Android packed relocation count");
    auto ELF = llvm::object::ELFFile<ELFT>::create(llvm::StringRef(
        reinterpret_cast<const char *>(Image.Raw.data()), Image.Raw.size()));
    if (!ELF)
      return ELF.takeError();
    ELFT::Shdr Section{};
    Section.sh_type = SHT_ANDROID_RELA;
    Section.sh_offset = B->data() - Image.Raw.data();
    Section.sh_size = Size;
    auto Relas = ELF->android_relas(Section);
    if (!Relas)
      return Relas.takeError();
    for (const auto &V : *Relas)
      if (auto E = Add(V.r_offset, V.r_addend, V.getType(false),
                       V.getSymbol(false), true))
        return std::move(E);
  }
  if (Get(DT_RELRSZ) && Get(DT_ANDROID_RELRSZ))
    return failure("multiple RELR encodings");
  int64_t Relr = Get(DT_RELRSZ) ? DT_RELR : DT_ANDROID_RELR;
  int64_t RelrSize = Relr == DT_RELR ? DT_RELRSZ : DT_ANDROID_RELRSZ;
  int64_t RelrEnt = Relr == DT_RELR ? DT_RELRENT : DT_ANDROID_RELRENT;
  if (uint64_t Size = Get(RelrSize)) {
    if (Get(RelrEnt) != 8 || Size % 8 ||
        Size / 8 > ::neverd::limits::loader::kELFProgramLinkingMaxRecords / 63)
      return failure("invalid RELR extent");
    auto B = R.bytes(Get(Relr), Size);
    if (!B)
      return B.takeError();
    uint32_t Relative;
    if (Image.Arch == Arch::AArch64)
      Relative = R_AARCH64_RELATIVE;
    else if (Image.Arch == Arch::X64)
      Relative = R_X86_64_RELATIVE;
    else
      return failure("RELR architecture unsupported");
    uint64_t Cursor = 0;
    bool HaveCursor = false;
    for (uint64_t I = 0; I < Size; I += 8) {
      uint64_t V = llvm::support::endian::read64le(B->data() + I);
      if (!(V & 1)) {
        if (V % 8 || V > UINT64_MAX - 8 || (HaveCursor && V < Cursor))
          return failure("invalid RELR address");
        if (auto E = Add(V, 0, Relative, 0, false))
          return std::move(E);
        Cursor = V + 8;
        HaveCursor = true;
      } else {
        if (!HaveCursor || Cursor > UINT64_MAX - 63 * 8)
          return failure("invalid RELR bitmap");
        for (unsigned Bit = 1; Bit < 64; ++Bit)
          if ((V >> Bit) & 1)
            if (auto E = Add(Cursor + (Bit - 1) * 8, 0, Relative, 0, false))
              return std::move(E);
        Cursor += 63 * 8;
      }
    }
  }
  uint64_t Count = 0;
  if (uint64_t Address = Get(DT_HASH)) {
    auto Header = R.bytes(Address, 8);
    if (!Header)
      return Header.takeError();
    uint64_t Buckets = llvm::support::endian::read32le(Header->data());
    Count = llvm::support::endian::read32le(Header->data() + 4);
    if (!Buckets ||
        Count > ::neverd::limits::loader::kELFProgramLinkingMaxRecords ||
        Buckets > ::neverd::limits::loader::kELFProgramLinkingMaxRecords)
      return failure("invalid symbol hash dimensions");
    auto Table = R.bytes(Address, 8 + 4 * (Buckets + Count));
    if (!Table)
      return Table.takeError();
  } else if (uint64_t Address = Get(DT_GNU_HASH)) {
    auto Header = R.bytes(Address, 16);
    if (!Header)
      return Header.takeError();
    uint64_t Buckets = llvm::support::endian::read32le(Header->data());
    uint64_t Start = llvm::support::endian::read32le(Header->data() + 4);
    uint64_t Bloom = llvm::support::endian::read32le(Header->data() + 8);
    if (!Buckets || !Bloom ||
        Buckets > ::neverd::limits::loader::kELFProgramLinkingMaxRecords ||
        Bloom > ::neverd::limits::loader::kELFProgramLinkingMaxRecords ||
        Start > ::neverd::limits::loader::kELFProgramLinkingMaxRecords)
      return failure("invalid GNU symbol hash dimensions");
    uint64_t Prefix = 16 + Bloom * 8 + Buckets * 4;
    auto Table = R.bytes(Address, Prefix);
    if (!Table)
      return Table.takeError();
    uint64_t Last = 0;
    for (uint64_t I = 0; I < Buckets; ++I) {
      uint64_t Index = llvm::support::endian::read32le(Table->data() + 16 +
                                                       Bloom * 8 + I * 4);
      if (Index &&
          (Index < Start ||
           Index >= ::neverd::limits::loader::kELFProgramLinkingMaxRecords))
        return failure("invalid GNU hash bucket");
      Last = std::max(Last, Index);
    }
    Count = Start;
    if (Last) {
      for (Count = Last;
           Count < ::neverd::limits::loader::kELFProgramLinkingMaxRecords;
           ++Count) {
        if (Address > UINT64_MAX - Prefix - (Count - Start) * 4)
          return failure("GNU hash chain overflows");
        auto Chain = R.bytes(Address + Prefix + (Count - Start) * 4, 4);
        if (!Chain)
          return Chain.takeError();
        if (llvm::support::endian::read32le(Chain->data()) & 1) {
          ++Count;
          break;
        }
      }
      if (Count >= ::neverd::limits::loader::kELFProgramLinkingMaxRecords)
        return failure("unterminated GNU hash chain");
    }
  } else if (Get(DT_SYMTAB)) {
    return failure("dynamic symbol table has no bounded hash table");
  }
  llvm::ArrayRef<uint8_t> Strings;
  if (Get(DT_STRSZ)) {
    auto B = R.bytes(Get(DT_STRTAB), Get(DT_STRSZ));
    if (!B)
      return B.takeError();
    Strings = *B;
  }
  uint64_t StringBudget =
      ::neverd::limits::loader::kELFProgramLinkingStringBudget;
  auto String = [&](uint64_t Offset) -> llvm::Expected<std::string> {
    if (Offset >= Strings.size())
      return failure("invalid dynamic string offset");
    const char *Begin = reinterpret_cast<const char *>(Strings.data() + Offset);
    const char *End = static_cast<const char *>(
        std::memchr(Begin, 0, Strings.size() - Offset));
    if (!End)
      return failure("unterminated dynamic string");
    if (uint64_t(End - Begin) > StringBudget)
      return failure("decoded dynamic strings exceed metadata budget");
    StringBudget -= End - Begin;
    return std::string(Begin, End);
  };
  if (Count) {
    if (Get(DT_SYMENT) != sizeof(ELFT::Sym))
      return failure("invalid dynamic symbol size");
    auto B = R.bytes(Get(DT_SYMTAB), Count * sizeof(ELFT::Sym));
    if (!B)
      return B.takeError();
    for (uint64_t I = 0; I < Count; ++I) {
      ELFT::Sym Sym;
      std::memcpy(&Sym, B->data() + I * sizeof(Sym), sizeof(Sym));
      auto Name = String(Sym.st_name);
      if (!Name)
        return Name.takeError();
      Out.Symbols.push_back({std::move(*Name), Sym.st_value, Sym.st_size,
                             Sym.st_shndx, Sym.st_info, Sym.st_other});
    }
  }
  for (const auto &Rel : Out.Relocations)
    if (Rel.Symbol && Rel.Symbol >= Count)
      return failure("relocation symbol out of bounds");
  for (const auto &D : Out.Dynamic) {
    if (D.Tag != DT_NEEDED)
      continue;
    auto Name = String(D.Value);
    if (!Name)
      return Name.takeError();
    Out.Needed.push_back(std::move(*Name));
  }
  return Out;
}
} // namespace neverd
