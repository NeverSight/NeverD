//===- KernelModelModulePublication.cpp - Atomic loader metadata ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Atomic loader metadata.
///

#include "../WindowsUnicode.h"
#include "../driver/DriverBootstrapModules.h"
#include "KernelExportRegistry.h"
#include "KernelModel.h"
#include "KernelSnapshotMemory.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/ModuleBinding.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
namespace module {
#define NEVERD_KERNEL_MODULE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "KernelLoadedModule.def"
#undef NEVERD_KERNEL_MODULE_VALUE
} // namespace module
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "kernel module publication: " + Text);
}
std::string basename(llvm::StringRef Path) {
  auto Name = Path.substr(Path.find_last_of('\\') + 1);
  auto Canonical = KernelExportRegistry::canonicalImportModule(Name);
  return Canonical.value_or(Name.lower());
}
} // namespace

class KernelModel::ModulePublicationState final : public ModulePublication {
public:
  explicit ModulePublicationState(KernelModel &M)
      : Owner(M), Rows(M.LoadedModules), Names(M.ModuleNameRanges),
        Allocations(M.ArenaAllocations), Cursor(M.NextAllocation) {}
  llvm::ArrayRef<BackingPatch> patches() const override { return Patches; }
  void commit() noexcept override {
    Owner.LoadedModules.swap(Rows);
    Owner.ModuleNameRanges.swap(Names);
    Owner.ArenaAllocations.swap(Allocations);
    Owner.NextAllocation = Cursor;
  }
  llvm::Expected<uint64_t> allocate(uint64_t Size) {
    auto Address =
        Owner.planArenaAllocation(Size, 16, Cursor, Owner.AllocationEnd);
    if (!Address)
      return Address.takeError();
    Allocations.emplace(*Address, Size);
    Cursor = *Address + Size;
    Storage.push_back({*Address, std::vector<uint8_t>(Size)});
    return *Address;
  }
  llvm::Expected<uint64_t> unicode(const std::string &Text) {
    auto Bytes = windows_unicode::bytes(Text, true);
    if (!Bytes)
      return Bytes.takeError();
    if (Bytes->size() > windows::MaxUnicodeBytes)
      return invalid("module name exceeds UNICODE_STRING capacity");
    auto Address = allocate(16 + Bytes->size());
    if (!Address)
      return Address.takeError();
    auto &Data = Storage.back().second;
    llvm::support::endian::write16le(Data.data(), Bytes->size() - 2);
    llvm::support::endian::write16le(Data.data() + 2, Bytes->size());
    llvm::support::endian::write64le(Data.data() + 8, *Address + 16);
    std::copy(Bytes->begin(), Bytes->end(), Data.begin() + 16);
    Names.emplace_back(*Address, Data.size());
    return *Address;
  }
  llvm::MutableArrayRef<uint8_t> data(uint64_t Address) {
    for (auto &[A, Bytes] : Storage)
      if (A == Address)
        return Bytes;
    llvm_unreachable("publication storage was lost");
  }
  void pointer(uint64_t At, uint64_t Value) {
    for (auto &[A, Bytes] : Storage)
      if (At >= A && Bytes.size() >= 8 && At - A <= Bytes.size() - 8) {
        llvm::support::endian::write64le(Bytes.data() + At - A, Value);
        return;
      }
    std::vector<uint8_t> Bytes(8);
    llvm::support::endian::write64le(Bytes.data(), Value);
    Storage.emplace_back(At, std::move(Bytes));
  }
  void finish() {
    Patches.reserve(Storage.size());
    for (const auto &[At, Bytes] : Storage)
      Patches.push_back({At, Bytes});
  }
  KernelModel &Owner;
  std::vector<LoadedModule> Rows;
  std::vector<std::pair<uint64_t, uint64_t>> Names;
  std::map<uint64_t, uint64_t> Allocations;
  uint64_t Cursor;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> Storage;
  std::vector<BackingPatch> Patches;
};

llvm::Expected<std::unique_ptr<ModulePublication>>
KernelModel::prepareModulePublication(llvm::ArrayRef<ModuleInstance> Preview) {
  if (!DriverModule || !DriverObject)
    return invalid("requires initialized driver loader ownership");
  auto Next = std::make_unique<ModulePublicationState>(*this);
  std::set<std::string> Seen, NewNames;
  for (const auto &M : LoadedModules)
    NewNames.insert(basename(M.Path));
  std::vector<uint64_t> Added;
  for (const auto &I : Preview) {
    if (!I.mapped())
      continue;
    if (!Seen.insert(I.Binding.ID).second ||
        (!I.Binding.Scope.empty() && I.Binding.Scope != "kernel"))
      return invalid("duplicate instance or unsupported scope");
    const auto Name =
        KernelExportRegistry::canonicalImportModule(I.Binding.GuestName);
    if (!Name)
      return invalid("guest identity requires a bounded PE basename");
    const auto Existing = std::find_if(
        LoadedModules.begin(), LoadedModules.end(),
        [&](const auto &M) { return M.InstanceID == I.Binding.ID; });
    if (Existing != LoadedModules.end()) {
      if (Existing->Base != I.Base || Existing->Size != I.Size ||
          Existing->Source != I.Source || basename(Existing->Path) != *Name)
        return invalid("an existing instance changed its publication identity");
      const bool Driver = Existing->Record == DriverModule;
      const bool Reference = !Existing->Guest && !Existing->Record;
      if (I.EnvironmentOwned != (Driver || Reference) ||
          (Driver &&
           (I.Binding.ID != DriverBootstrapID || I.Role != "driver")) ||
          (Reference && (I.Binding.ID != KernelReferenceBootstrapID ||
                         I.Role != "kernel-reference")) ||
          (!I.EnvironmentOwned && !I.Role.empty()))
        return invalid(
            "an existing instance changed its environment ownership");
      continue;
    }
    if (I.EnvironmentOwned) {
      const bool Driver =
          I.Binding.ID == DriverBootstrapID && I.Role == "driver";
      const bool Reference = I.Binding.ID == KernelReferenceBootstrapID &&
                             I.Role == "kernel-reference" && !I.Entry;
      auto Row = std::find_if(
          Next->Rows.begin(), Next->Rows.end(), [&](const auto &M) {
            return M.InstanceID.empty() && !M.Source && M.Base == I.Base &&
                   M.Size == I.Size && basename(M.Path) == *Name &&
                   ((Driver && M.Guest && M.Record == DriverModule) ||
                    (Reference && !M.Guest && !M.Record));
          });
      if (Row == Next->Rows.end())
        return invalid("preexisting instance has no matching kernel owner");
      Row->InstanceID = I.Binding.ID;
      Row->Source = I.Source;
      continue;
    }
    if (!I.Role.empty())
      return invalid("new guest module cannot claim an environment role");
    if (!NewNames.insert(*Name).second)
      return invalid("guest module name aliases an existing kernel identity");
    auto Object = llvm::object::COFFObjectFile::create(
        llvm::MemoryBufferRef(llvm::StringRef(reinterpret_cast<const char *>(
                                                  I.Source->bytes().data()),
                                              I.Source->bytes().size()),
                              "kernel-module-publication"));
    if (!Object)
      return Object.takeError();
    const auto *Header = (*Object)->getPE32PlusHeader();
    if (!Header || I.Source->image().Arch != Arch::X64 ||
        Header->Subsystem != llvm::COFF::IMAGE_SUBSYSTEM_NATIVE ||
        I.Size != Header->SizeOfImage || !I.Size || I.Size > UINT32_MAX ||
        I.Base > UINT64_MAX - I.Size || I.Base % 4096 ||
        I.Entry != (Header->AddressOfEntryPoint
                        ? I.Base + Header->AddressOfEntryPoint
                        : 0))
      return invalid("source and mapped image identities disagree");
    for (const auto &M : Next->Rows)
      if (I.Base <= M.Base ? M.Base - I.Base < I.Size
                           : I.Base - M.Base < M.Size)
        return invalid("module overlaps an existing kernel image owner");
    const std::string FullName =
        "\\SystemRoot\\system32\\drivers\\" + I.Binding.GuestName;
    if (FullName.size() >= 256)
      return invalid("module path exceeds SystemModuleInformation capacity");
    auto Record = Next->allocate(module::Size);
    if (!Record)
      return Record.takeError();
    auto Full = Next->unicode(FullName),
         Base = Next->unicode(I.Binding.GuestName);
    if (!Full || !Base)
      return llvm::joinErrors(Full.takeError(), Base.takeError());
    auto Bytes = Next->data(*Record);
    llvm::support::endian::write64le(Bytes.data() + module::ImageBase, I.Base);
    llvm::support::endian::write64le(Bytes.data() + module::EntryPoint,
                                     I.Entry);
    llvm::support::endian::write32le(Bytes.data() + module::ImageSize, I.Size);
    llvm::support::endian::write16le(Bytes.data() + module::LoadCount, 1);
    std::copy_n(Next->data(*Full).begin(), 16,
                Bytes.begin() + module::FullName);
    std::copy_n(Next->data(*Base).begin(), 16,
                Bytes.begin() + module::BaseName);
    Next->Rows.push_back({I.Base, I.Size, FullName, true,
                          Header->DLLCharacteristics, *Record, I.Binding.ID,
                          I.Source});
    Added.push_back(*Record);
  }
  for (const auto &M : LoadedModules)
    if (!M.InstanceID.empty() && !Seen.contains(M.InstanceID))
      return invalid("withdrawing a live instance is unsupported");
  if (!Added.empty()) {
    KernelSnapshotMemory Observation(Memory);
    // Validate the live ring, including service-owned records, before planning
    // a tail append. Never repair guest-corrupted links as a side effect.
    std::set<uint64_t> Allowed{DriverModule}, Visited;
    for (const auto &M : LoadedModules)
      if (M.Record)
        Allowed.insert(M.Record);
    uint64_t Current = DriverModule, Last = 0;
    do {
      if (!Allowed.contains(Current) || !Visited.insert(Current).second)
        return invalid("live loader ring has an unknown record or cycle");
      auto Forward = Observation.readInteger(Current, 8);
      auto Back = Observation.readInteger(Current + 8, 8);
      if (!Forward || !Back)
        return llvm::joinErrors(Forward.takeError(), Back.takeError());
      if (Last && *Back != Last)
        return invalid("live loader ring has inconsistent backward links");
      Last = Current;
      Current = *Forward;
    } while (Current != DriverModule);
    auto Tail = Observation.readInteger(DriverModule + 8, 8);
    if (!Tail)
      return Tail.takeError();
    if (*Tail != Last || Visited != Allowed)
      return invalid(
          "live loader ring omits owned records or has a stale tail");
    for (size_t N = 0; N < Added.size(); ++N) {
      Next->pointer(Added[N],
                    N + 1 == Added.size() ? DriverModule : Added[N + 1]);
      Next->pointer(Added[N] + 8, N ? Added[N - 1] : Last);
    }
    Next->pointer(Last, Added.front());
    Next->pointer(DriverModule + 8, Added.back());
  }
  Next->finish();
  return std::unique_ptr<ModulePublication>(std::move(Next));
}
} // namespace neverd::emulation
