//===- ObjectExterns.cpp - Addresses of a relocatable object's externs ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lays out the extern segment of a relocatable object and adds it to the
/// image.  Another module defines what the segment holds, so it is writable
/// and states nothing.  A called extern's address is the import's callable
/// identity, as a Mach-O stub's is; data is a variable another module
/// defines: a symbol, and no import, whose address would read as an import
/// slot.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/ObjectExterns.h"

#include "neverd/Limits.h"
#include "neverd/object/SectionNames.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace neverd {
namespace object_externs {

namespace {

/// Alignment of the extern segment, of the cells, and of each data extern.
constexpr uint64_t kExternAlignment = 16;

llvm::Error noExternRoom() {
  return llvm::make_error<llvm::StringError>(
      "loader: no address room past the object's sections for its externs",
      llvm::inconvertibleErrorCode());
}

/// Advance \p Next past one item of \p Bytes aligned to \p Alignment and
/// return the item's address, or nullopt when the address space ends first.
std::optional<va_t> allocate(va_t &Next, uint64_t Bytes, uint64_t Alignment) {
  if (Next > InvalidVA - (Alignment - 1))
    return std::nullopt;
  const va_t At = llvm::alignTo(Next, Alignment);
  if (Bytes > InvalidVA - At)
    return std::nullopt;
  Next = At + Bytes;
  return At;
}

void addRegion(llvm::StringRef Name, va_t VA, uint64_t Bytes,
               SegmentFlags Flags, std::vector<uint8_t> Contents,
               BinaryImage &Img) {
  Section Sec;
  Sec.Name = Name.str();
  Sec.VA = VA;
  Sec.Size = Bytes;
  Sec.Flags = Flags;
  Sec.Alignment = kExternAlignment;
  Segment Seg;
  Seg.Name = Name.str();
  Seg.VA = VA;
  Seg.Size = Bytes;
  Seg.Flags = Flags;
  if (hasFlag(Flags, SegmentFlags::Writable)) {
    // Zero-filled like an object's .bss: the object states nothing here.
    Sec.Type = llvm::ELF::SHT_NOBITS;
    Seg.Data.assign(static_cast<size_t>(Bytes), 0);
  } else {
    Sec.Type = llvm::ELF::SHT_PROGBITS;
    Sec.FileSz = Bytes;
    Sec.Data = Contents;
    Seg.FileSz = Bytes;
    Seg.Data = std::move(Contents);
    Seg.ReadOnlyAfterRelocations = true;
  }
  Img.Sections.push_back(std::move(Sec));
  Img.Segments.push_back(std::move(Seg));
}

} // namespace

void ExternRequests::noteUndefined(llvm::StringRef Name, bool Called,
                                   int64_t Addend) {
  if (Name.empty())
    return;
  auto [It, Inserted] =
      UndefinedIndex.try_emplace(Name.str(), Undefined.size());
  if (Inserted)
    Undefined.push_back({Name.str()});
  UndefinedSymbol &U = Undefined[It->second];
  U.Called |= Called;
  if (Addend > 0)
    U.StatedReach = std::max(U.StatedReach, static_cast<uint64_t>(Addend));
}

void ExternRequests::noteCommon(llvm::StringRef Name, uint64_t Bytes,
                                uint64_t Alignment) {
  if (!Name.empty() && CommonNames.insert(Name.str()).second)
    Commons.push_back(
        {Name.str(), Bytes, llvm::isPowerOf2_64(Alignment) ? Alignment : 1});
}

llvm::Expected<ExternLayout> layoutExterns(const ExternRequests &Requests,
                                           size_t Cells, uint64_t PointerBytes,
                                           va_t ImageEnd) {
  ExternLayout Layout;
  Layout.PointerBytes = PointerBytes;
  if (Requests.empty() && Cells == 0)
    return Layout;
  va_t Next = ImageEnd;
  auto Base = allocate(Next, 0, kExternAlignment);
  if (!Base)
    return noExternRoom();
  Layout.ExternBase = *Base;
  for (const auto &U : Requests.Undefined) {
    // A function is called, never read at an offset.  Data keeps room past
    // its address, so a field another module defines is not another extern.
    uint64_t Bytes = PointerBytes, Alignment = PointerBytes;
    if (!U.Called) {
      Bytes = std::min(U.StatedReach, limits::kMaxObjectExternStatedReach) +
              limits::kObjectExternDataReach;
      Alignment = kExternAlignment;
    }
    auto At = allocate(Next, Bytes, Alignment);
    if (!At)
      return noExternRoom();
    Layout.SymbolSlots[U.Name] = *At;
    if (U.Called)
      Layout.CalledSymbols.insert(U.Name);
    else
      Layout.SymbolBytes[U.Name] = Bytes;
  }
  for (const auto &C : Requests.Commons) {
    auto At = allocate(Next, std::max<uint64_t>(C.Bytes, 1),
                       std::max(C.Alignment, kExternAlignment));
    if (!At)
      return noExternRoom();
    Layout.CommonSlots[C.Name] = *At;
    Layout.CommonSizes[C.Name] = C.Bytes;
  }
  Layout.ExternSize = Next - Layout.ExternBase;
  if (Cells != 0) {
    auto CellBase = allocate(Next, 0, kExternAlignment);
    if (!CellBase || Cells > (InvalidVA - *CellBase) / PointerBytes)
      return noExternRoom();
    Layout.CellBase = *CellBase;
    Layout.CellSize = Cells * PointerBytes;
  }
  return Layout;
}

void addExternSegment(
    const ExternLayout &Layout, BinaryImage &Img,
    llvm::function_ref<std::string(llvm::StringRef)> ImportName) {
  if (Layout.ExternSize == 0)
    return;
  addRegion(section_names::SynthesizedExtern, Layout.ExternBase,
            Layout.ExternSize, SegmentFlags::Readable | SegmentFlags::Writable,
            {}, Img);
  for (const auto &[Name, SlotVA] : Layout.SymbolSlots) {
    const bool Called = Layout.CalledSymbols.count(Name) != 0;
    if (Called) {
      Import Imp;
      Imp.Module = kExternModule.str();
      Imp.Name = ImportName(Name);
      Imp.IATAddr = SlotVA;
      Img.Imports.push_back(std::move(Imp));
      Img.ObjectExternFunctionEntries.insert(SlotVA);
    } else if (const auto Bytes = Layout.SymbolBytes.find(Name);
               Bytes != Layout.SymbolBytes.end()) {
      Img.ObjectExternDataSymbols[SlotVA] = {Name, Bytes->second};
    }
    // Another module states the size of what it defines; the object does
    // not, so the symbol has none.
    Symbol Sym;
    Sym.Name = Name;
    Sym.Addr = SlotVA;
    Sym.IsFunc = Called;
    Img.Symbols.push_back(std::move(Sym));
  }
}

void addCellSegment(const ExternLayout &Layout, llvm::StringRef Name,
                    std::vector<uint8_t> Contents, BinaryImage &Img) {
  if (Layout.CellSize == 0)
    return;
  Contents.resize(static_cast<size_t>(Layout.CellSize), 0);
  addRegion(Name, Layout.CellBase, Layout.CellSize, SegmentFlags::Readable,
            std::move(Contents), Img);
}

void writeCell(const ExternLayout &Layout, std::vector<uint8_t> &Contents,
               size_t Index, va_t Address) {
  const size_t Offset = static_cast<size_t>(Index * Layout.PointerBytes);
  if (Contents.size() < Offset + Layout.PointerBytes)
    Contents.resize(Offset + static_cast<size_t>(Layout.PointerBytes), 0);
  const uint64_t Value = Address;
  std::memcpy(Contents.data() + Offset, &Value,
              static_cast<size_t>(Layout.PointerBytes));
}

} // namespace object_externs
} // namespace neverd
