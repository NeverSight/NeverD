//===- DriverCPUID.cpp - Declared CPUID register observations -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DriverCPUID.h"

#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation::driver_cpuid {
namespace {
constexpr llvm::StringLiteral RegisterNames[] = {"eax", "ebx", "ecx", "edx"};

llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError("driver cpuid: " + Text);
}

llvm::Expected<uint32_t> word(const llvm::json::Object &Object,
                              llvm::StringRef Name) {
  auto Value = Object.getInteger(Name);
  if (!Value || *Value < 0 || uint64_t(*Value) > UINT32_MAX)
    return invalid(Name + " must be a 32-bit unsigned integer");
  return uint32_t(*Value);
}
} // namespace

llvm::Error validate(llvm::ArrayRef<DriverCPUID> Entries) {
  if (Entries.size() > profile::MaxCPUIDEntries)
    return invalid("inventory exceeds the declared entry limit");
  for (size_t I = 0; I < Entries.size(); ++I)
    for (size_t J = 0; J < I; ++J)
      if (Entries[I].Leaf == Entries[J].Leaf &&
          (!Entries[I].Subleaf || !Entries[J].Subleaf ||
           Entries[I].Subleaf == Entries[J].Subleaf))
        return invalid("duplicate or overlapping leaf/subleaf declarations");
  return llvm::Error::success();
}

llvm::Expected<std::vector<DriverCPUID>> parse(const llvm::json::Value &Value) {
  const auto *Array = Value.getAsArray();
  if (!Array || Array->size() > profile::MaxCPUIDEntries)
    return invalid("inventory must be a bounded array");
  std::vector<DriverCPUID> Entries;
  for (const auto &Item : *Array) {
    const auto *Object = Item.getAsObject();
    if (!Object)
      return invalid("each declaration must be an object");
    for (const auto &[Key, Unused] : *Object)
      if (Key != "leaf" && Key != "subleaf" && Key != "eax" && Key != "ebx" &&
          Key != "ecx" && Key != "edx")
        return invalid("unknown declaration field " + Key.str());
    DriverCPUID Entry;
    auto Leaf = word(*Object, "leaf");
    if (!Leaf)
      return Leaf.takeError();
    Entry.Leaf = *Leaf;
    if (Object->get("subleaf")) {
      auto Subleaf = word(*Object, "subleaf");
      if (!Subleaf)
        return Subleaf.takeError();
      Entry.Subleaf = *Subleaf;
    }
    for (unsigned I = 0; I < Entry.Registers.size(); ++I) {
      auto Value = word(*Object, RegisterNames[I]);
      if (!Value)
        return Value.takeError();
      Entry.Registers[I] = *Value;
    }
    Entries.push_back(Entry);
  }
  if (auto E = validate(Entries))
    return std::move(E);
  return Entries;
}

llvm::json::Array toJSON(llvm::ArrayRef<DriverCPUID> Entries) {
  llvm::json::Array Array;
  for (const auto &Entry : Entries) {
    llvm::json::Object Item{{"leaf", Entry.Leaf}};
    if (Entry.Subleaf)
      Item["subleaf"] = *Entry.Subleaf;
    for (unsigned I = 0; I < Entry.Registers.size(); ++I)
      Item[RegisterNames[I]] = Entry.Registers[I];
    Array.push_back(std::move(Item));
  }
  return Array;
}

llvm::Expected<std::array<uint32_t, 4>>
query(llvm::ArrayRef<DriverCPUID> Entries, uint32_t Leaf, uint32_t Subleaf) {
  for (const auto &Entry : Entries)
    if (Entry.Leaf == Leaf && (!Entry.Subleaf || *Entry.Subleaf == Subleaf))
      return Entry.Registers;
  return invalid(
      llvm::formatv("undeclared leaf {0:x}, subleaf {1:x}", Leaf, Subleaf)
          .str());
}
} // namespace neverd::emulation::driver_cpuid
