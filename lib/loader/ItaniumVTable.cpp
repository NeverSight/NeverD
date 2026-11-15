//===- ItaniumVTable.cpp - Primary virtual table declarations ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover primary virtual-table targets from loaded Itanium ABI symbols.
//===----------------------------------------------------------------------===//

#include "neverd/loader/ItaniumVTable.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/Demangle/Demangle.h"

using namespace neverd;

ItaniumVTableIndex::ItaniumVTableIndex(const BinaryImage &Image) {
  for (const auto &Symbol : Image.Symbols)
    add(Image, Symbol);
}

void ItaniumVTableIndex::add(const BinaryImage &Image, const Symbol &Symbol) {
  if (Symbol.IsFunc || Symbol.IsSection)
    return;
  const auto Name = cNameOfSymbol(Symbol.Name, Image.Format, Image.Arch);
  if (Name.starts_with("_ZTI")) {
    auto [At, Fresh] = TypeInfos.emplace(Symbol.Addr, &Symbol);
    if (!Fresh && At->second && At->second->Name != Symbol.Name)
      At->second = nullptr;
  }
  if (!Name.starts_with("_ZTV"))
    return;
  const std::string Demangled = llvm::demangle(Name.str());
  llvm::StringRef Class(Demangled);
  if (!Class.consume_front("vtable for "))
    return;
  auto [At, Fresh] = Tables.emplace(Class.str(), &Symbol);
  if (!Fresh && At->second && At->second->Addr != Symbol.Addr)
    At->second = nullptr;
}

std::optional<ItaniumVirtualSlot>
ItaniumVTableIndex::resolve(const BinaryImage &Image, const std::string &Class,
                            uint64_t Offset) const {
  if (Image.Format != BinaryFormat::ELF && Image.Format != BinaryFormat::MachO)
    return {};
  const unsigned Bytes = Image.getPointerSize();
  if (Bytes != 4 && Bytes != 8)
    return {};
  const auto Found = Tables.find(Class);
  if (Found == Tables.end() || !Found->second)
    return {};
  const Symbol &Table = *Found->second;
  const auto Read = [&](va_t Address) {
    const auto *Data = Image.readVA(Address, Bytes);
    return Data ? readPtr(Data, Image.is64Bit(), archLittleEndian(Image.Arch))
                : InvalidVA;
  };
  if (Table.Size < 3 * Bytes || Offset % Bytes ||
      Offset > Table.Size - 3 * Bytes || Table.Addr > InvalidVA - Table.Size ||
      Read(Table.Addr) != 0 ||
      !Image.readOnlyAfterRelocations(Table.Addr, 3 * Bytes + Offset))
    return {};
  const va_t RTTI = Read(Table.Addr + Bytes);
  if (RTTI) {
    const auto Info = TypeInfos.find(RTTI);
    const auto Name = cNameOfSymbol(Table.Name, Image.Format, Image.Arch);
    if (Info == TypeInfos.end() || !Info->second ||
        !Image.isDataAddress(RTTI) ||
        cNameOfSymbol(Info->second->Name, Image.Format, Image.Arch) !=
            "_ZTI" + Name.drop_front(4).str())
      return {};
  }
  const va_t AddressPoint = Table.Addr + 2 * Bytes;
  for (uint64_t I = 0; I <= Offset; I += Bytes) {
    const va_t Slot = AddressPoint + I;
    if (!Image.CodePtrRelocSlots.count(Slot) ||
        !Image.hasExecutableCodeOwnerAt(Read(Slot)))
      return {};
  }
  return ItaniumVirtualSlot{Table.Addr, AddressPoint + Offset,
                            normalizeCodeAddress(Read(AddressPoint + Offset),
                                                 Image.Arch, Image.Mode)};
}
