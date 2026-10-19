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
  if (!Array)
    return invalid("inventory must be a bounded array");
  if (Array->size() > profile::MaxCPUIDEntries)
    return invalid("inventory exceeds the declared entry limit");
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

//===- DriverCPUID.cpp - Validated guest CPUID query ownership
//-------------===//
#include "neverd/emulation/DriverCPUID.h"
#include "neverd/emulation/DriverSession.h"

namespace neverd::emulation {
namespace {
llvm::Error invalid(const char *Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "cpuid_policy: %s", Text);
}
bool neverDBlock(uint32_t Leaf) {
  return Leaf >= 0x40000000 && Leaf <= 0x400000ff;
}
} // namespace
DriverCPUIDRegisters DriverCPUIDModel::neverDV1(uint32_t Leaf) {
  if (Leaf == 0x40000000)
    return {0x40000001, 0x6576654e, 0x45204472, 0x2020756d}; // NeverD Emu
  if (Leaf == 0x40000001)
    return {0x3123444e, 0, 0,
            0}; // ND#1: NeverD's own query-only discovery ABI.
  return {};
}
bool DriverCPUIDModel::invalidArchitecturalLeaf(uint32_t Leaf) const {
  // Hypervisor namespaces have their own policies; never apply hardware's
  // invalid-leaf fallback to an unmodeled virtual interface.
  if (Leaf >= 0x40000000 && Leaf < 0x50000000)
    return false;
  return Leaf < 0x80000000 ? Leaf > MaximumBasic : Leaf > MaximumExtended;
}
llvm::Expected<DriverCPUIDModel>
DriverCPUIDModel::create(const DriverCPUIDTable &Facts,
                         const std::optional<DriverCPUIDPolicy> &P) {
  if (Facts.size() > 256)
    return invalid("cpuid exceeds the 256-query limit");
  DriverCPUIDModel M;
  M.Observations = Facts;
  if (P)
    M.Policy = *P;
  switch (M.Policy.InvalidLeaf) {
  case DriverCPUIDInvalidLeafPolicy::Strict:
    break;
  case DriverCPUIDInvalidLeafPolicy::IntelMaxBasicV1: {
    auto Basic = Facts.find({0, 0}), Extended = Facts.find({0x80000000, 0});
    if (Basic == Facts.end() || Extended == Facts.end())
      return invalid(
          "intel-max-basic-v1 requires basic and extended root observations");
    const auto &B = Basic->second;
    if (B[1] != 0x756e6547 || B[3] != 0x49656e69 || B[2] != 0x6c65746e)
      return invalid("intel-max-basic-v1 requires GenuineIntel");
    // This version is bounded to the observed pre-0x21 Intel basic namespace.
    // Newer discovery revisions remain explicit until separately modeled.
    if (B[0] > 0x20 || Extended->second[0] < 0x80000000 ||
        Extended->second[0] >= 0xc0000000)
      return invalid("intel-max-basic-v1 requires basic maximum <=0x20 and a "
                     "bounded extended range");
    M.MaximumBasic = B[0];
    M.MaximumExtended = Extended->second[0];
    if (!Facts.contains({M.MaximumBasic, 0}))
      return invalid(
          "intel-max-basic-v1 requires the maximum basic leaf observation");
    for (const auto &[Input, Output] : Facts)
      if (M.invalidArchitecturalLeaf(Input.first)) {
        auto Maximum = Facts.find({M.MaximumBasic, Input.second});
        if (Maximum == Facts.end() || Maximum->second != Output)
          return invalid("invalid-leaf observation conflicts with maximum "
                         "basic leaf/subleaf");
      }
    break;
  }
  default:
    return invalid("invalid invalid-leaf policy");
  }
  switch (M.Policy.Hypervisor) {
  case DriverCPUIDHypervisorPolicy::Observed:
    break;
  case DriverCPUIDHypervisorPolicy::NeverDV1: {
    auto Features = Facts.find({1, 0});
    if (Features == Facts.end() || !(Features->second[2] & (uint32_t(1) << 31)))
      return invalid("neverd-v1 requires CPUID(1,0).ECX hypervisor-present");
    for (const auto &[Input, Output] : Facts)
      if (neverDBlock(Input.first) && Output != neverDV1(Input.first))
        return invalid("observation conflicts with neverd-v1 discovery ABI");
    break;
  }
  default:
    return invalid("invalid hypervisor policy");
  }
  return M;
}
std::optional<DriverCPUIDRegisters>
DriverCPUIDModel::query(uint32_t Leaf, uint32_t Subleaf) const {
  if (auto I = Observations.find({Leaf, Subleaf}); I != Observations.end())
    return I->second;
  if (auto I = WildcardObservations.find(Leaf); I != WildcardObservations.end())
    return I->second;
  if (Policy.Hypervisor == DriverCPUIDHypervisorPolicy::NeverDV1 &&
      neverDBlock(Leaf))
    return neverDV1(Leaf);
  if (Policy.InvalidLeaf == DriverCPUIDInvalidLeafPolicy::IntelMaxBasicV1 &&
      invalidArchitecturalLeaf(Leaf))
    if (auto I = Observations.find({MaximumBasic, Subleaf});
        I != Observations.end())
      return I->second;
  return std::nullopt;
}
llvm::Expected<DriverCPUIDModel>
DriverCPUIDModel::fromDriverOptions(const DriverOptions &Options) {
  if (auto E = driver_cpuid::validate(Options.CPUID))
    return E;
  DriverCPUIDTable Facts;
  for (const auto &Observation : Options.CPUID)
    Facts.emplace(std::pair{Observation.Leaf, Observation.Subleaf.value_or(0)},
                  Observation.Registers);
  auto Model = create(Facts, Options.CPUIDPolicy);
  if (!Model)
    return Model.takeError();
  for (const auto &Observation : Options.CPUID)
    if (!Observation.Subleaf)
      Model->WildcardObservations.emplace(Observation.Leaf,
                                          Observation.Registers);
  return Model;
}
} // namespace neverd::emulation
