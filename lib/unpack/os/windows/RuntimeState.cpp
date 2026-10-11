//===- RuntimeState.cpp - Compile a bounded Windows state initializer -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "RuntimeState.h"

#include "../../format/pe/PEImage.h"
#include "RuntimeSource.h"

#include "neverd/emulation/WindowsProcessState.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <map>
#include <set>

namespace neverd::unpack::windows {
namespace {
using emulation::WindowsProcessState;
constexpr uint64_t ReservationAlignment = 65536;
constexpr uint64_t PageSize = 4096;
constexpr uint64_t MaxBacking = 32 * 1024 * 1024;
constexpr uint64_t MaxBindings = 16384;
struct Region {
  uint64_t Address, Size;
};
using Row = std::vector<std::string>;
using Rows = std::vector<Row>;
std::string integer(uint64_t V) { return "0x" + llvm::utohexstr(V) + "ULL"; }
std::string string(llvm::StringRef V) {
  std::string S = "\"";
  for (uint8_t C : V.bytes()) {
    S += "\\x";
    S += llvm::hexdigit(C >> 4);
    S += llvm::hexdigit(C & 15);
  }
  return S + '"';
}
bool validName(llvm::StringRef S) {
  return !S.empty() && S.size() <= 4096 &&
         llvm::all_of(S.bytes(), [](uint8_t C) { return C >= 32 && C < 127; });
}
llvm::Expected<std::vector<Region>> regions(llvm::ArrayRef<Region> Input) {
  std::set<uint64_t> Pages;
  for (const auto &R : Input) {
    if (!R.Size || R.Address < ReservationAlignment ||
        R.Address >= (1ull << 47) || R.Size > (1ull << 47) - R.Address)
      return failure("native state has an invalid user-address range");
    const uint64_t End =
        llvm::alignTo(R.Address + R.Size, ReservationAlignment);
    for (uint64_t P = llvm::alignDown(R.Address, ReservationAlignment); P < End;
         P += ReservationAlignment) {
      Pages.insert(P);
      if (Pages.size() > MaxBacking / ReservationAlignment)
        return failure("native state reservation limit exceeded");
    }
  }
  std::vector<Region> Result;
  for (uint64_t P : Pages) {
    if (!Result.empty() && Result.back().Address + Result.back().Size == P)
      Result.back().Size += ReservationAlignment;
    else
      Result.push_back({P, ReservationAlignment});
  }
  return Result;
}
llvm::Error write(const std::filesystem::path &Path, llvm::StringRef Text) {
  std::ofstream F(Path, std::ios::binary);
  F.write(Text.data(), Text.size());
  F.close();
  if (!F)
    return failure("cannot write native runtime compiler input");
  return llvm::Error::success();
}
struct Scratch {
  llvm::SmallString<128> Path;
  ~Scratch() {
    if (!Path.empty())
      llvm::sys::fs::remove_directories(Path);
  }
};
llvm::Expected<std::vector<uint8_t>> compile(llvm::StringRef Header,
                                             uint64_t Base) {
  auto Clang = llvm::sys::findProgramByName("clang");
  auto Link = llvm::sys::findProgramByName("lld-link");
  if (!Clang || !Link)
    return failure("runtime restoration requires clang and lld-link in PATH");
  Scratch Temporary;
  if (auto E = llvm::sys::fs::createUniqueDirectory("neverd-runtime",
                                                    Temporary.Path))
    return failure(E.message());
  const std::filesystem::path D(Temporary.Path.str().str());
  if (auto E = write(D / "runtime.c", NativeRuntimeSource))
    return std::move(E);
  if (auto E = write(D / "payload.h", Header))
    return std::move(E);
  std::string Definition = "LIBRARY kernel32.dll\nEXPORTS\n";
#define NEVERD_RUNTIME_IMPORT(Name) Definition += #Name "\n";
#include "RuntimeImports.def"
#undef NEVERD_RUNTIME_IMPORT
  if (auto E = write(D / "kernel.def", Definition))
    return std::move(E);
  const std::string Def = "/def:" + (D / "kernel.def").string();
  const std::string Lib = (D / "kernel.lib").string();
  const std::string LibOut = "/out:" + Lib;
  const std::string Source = (D / "runtime.c").string();
  const std::string Object = (D / "runtime.obj").string();
  const std::string DLL = "/out:" + (D / "runtime.dll").string();
  const std::string ImageBase = "/base:0x" + llvm::utohexstr(Base);
  auto Run = [&](llvm::ArrayRef<llvm::StringRef> Args) -> llvm::Error {
    const std::string Log = (D / "compiler.log").string();
    const std::optional<llvm::StringRef> Redirects[] = {llvm::StringRef(), Log,
                                                        Log};
    std::string Error;
    bool Failed = false;
    const int Status = llvm::sys::ExecuteAndWait(
        Args.front(), Args, std::nullopt, Redirects, 60, 0, &Error, &Failed);
    if (!Status && !Failed)
      return llvm::Error::success();
    std::ifstream F(Log, std::ios::binary);
    char Bytes[4096];
    F.read(Bytes, sizeof(Bytes));
    return failure("native runtime compilation failed: " + Error + " " +
                   std::string(Bytes, F.gcount()));
  };
  if (auto E = Run({*Link, "/lib", "/machine:x64", Def, LibOut}))
    return std::move(E);
  if (auto E = Run({*Clang, "--target=x86_64-pc-windows-msvc", "-ffreestanding",
                    "-fno-builtin", "-fno-stack-protector", "-O1", "-c", Source,
                    "-o", Object}))
    return std::move(E);
  if (auto E = Run({*Link, "/nodefaultlib", "/dll", "/entry:restore", "/fixed",
                    "/dynamicbase:no", "/timestamp:0", "/merge:.idata=.data",
                    ImageBase, Object, Lib, DLL}))
    return std::move(E);
  return readInput(D / "runtime.dll");
}
} // namespace

llvm::Expected<RebuiltImage> restoreRuntime(const InputImage &Input,
                                            const Capture &C,
                                            RebuiltImage Rebuilt) {
  if (Input.format() != FormatKind::PE64 ||
      Input.architecture() != emulation::GuestArchitecture::X64)
    return failure("native runtime materialization requires Windows x64 PE32+");
  const auto &S = static_cast<const WindowsProcessState &>(*C.OwnedState);
  if (!S.Version)
    return failure(
        "native runtime materialization requires explicit windows.peb_version");
  if (S.HasOpenFiles || S.HasSections || S.HasMappedViews ||
      S.HasThreadSnapshots || S.HasExceptionState || S.HasActiveFiberCleanup ||
      S.HasUnownedThreadSlots || !S.ThreadSlots.empty())
    return failure("native state retains an unsupported handle, mapping, "
                   "exception or TLS owner");
  if (S.Allocations.size() > 8192 || S.PrivateHeaps.size() > 4096 ||
      S.CriticalSections.size() > 4096 || S.FiberSlots.size() > 64 ||
      S.VirtualAllocations.size() > 8192 || C.Exports.size() > MaxBindings)
    return failure("native state object inventory limit exceeded");
  uint64_t RuntimeRVA = 0;
  for (const auto &R : Rebuilt.Sections) {
    if (R.RVA > UINT32_MAX || R.VirtualSize > UINT32_MAX - R.RVA)
      return failure("native state image extent is invalid");
    RuntimeRVA = std::max(RuntimeRVA, R.RVA + R.VirtualSize);
  }
  RuntimeRVA = llvm::alignTo(RuntimeRVA, ReservationAlignment);
  if (RuntimeRVA > UINT32_MAX || C.Base > (1ull << 47) - RuntimeRVA)
    return failure("native state runtime placement is invalid");
  std::string Header;
  llvm::raw_string_ostream O(Header);
  auto Define = [&](llvm::StringRef Name, uint64_t Value) {
    O << "#define " << Name << ' ' << integer(Value) << '\n';
  };
  auto Table = [&](llvm::StringRef Type, llvm::StringRef Name,
                   llvm::StringRef Count, const Rows &Items) {
    Define(Count, Items.size());
    O << "static " << Type << ' ' << Name << "[]={\n";
    if (Items.empty())
      O << "{0},\n";
    for (const auto &R : Items) {
      O << '{';
      for (size_t I = 0; I < R.size(); ++I)
        O << (I ? "," : "") << R[I];
      O << "},\n";
    }
    O << "};\n";
  };
  Define("IMAGE_BASE", C.Base);
  Define("LOADER_ENTRY", C.Base + Rebuilt.LoaderEntryRVA);
  Define("PROCESS_HEAP", S.ProcessHeap);
  Define("POINTER_COOKIE", S.PointerXor);
  Define("LAST_ERROR", S.LastError);
  Define("VERSION_MAJOR", S.Version->Major);
  Define("VERSION_MINOR", S.Version->Minor);
  Define("VERSION_BUILD", S.Version->Build);
  Define("VERSION_PLATFORM", S.Version->Platform);
  std::set<uint64_t> Heaps{S.ProcessHeap};
  Heaps.insert(S.PrivateHeaps.begin(), S.PrivateHeaps.end());
  Rows HeapRows, Blocks;
  for (uint64_t H : Heaps)
    HeapRows.push_back({integer(H), "0"});
  Table("Heap", "Heaps", "HEAP_COUNT", HeapRows);
  uint64_t PreviousEnd = 0;
  std::vector<Region> BlocksRanges, GateRanges;
  std::vector<uint8_t> Payload;
  for (const auto &B : S.Allocations) {
    if (B.Address % PageSize || B.Bytes.empty() || B.Bytes.size() % PageSize ||
        B.Bytes.size() > MaxBacking - Payload.size() ||
        B.RequestedSize > B.Bytes.size() || !Heaps.contains(B.Heap) ||
        B.Address < PreviousEnd || B.Bytes.size() > UINT64_MAX - B.Address)
      return failure("native heap inventory has invalid ownership or backing");
    PreviousEnd = B.Address + B.Bytes.size();
    Blocks.push_back({integer(B.Address), integer(B.RequestedSize),
                      integer(B.Bytes.size()), integer(B.Heap),
                      integer(Payload.size()),
                      B.EnvironmentSnapshot ? "1" : "0", "1"});
    BlocksRanges.push_back({B.Address, B.Bytes.size()});
    Payload.insert(Payload.end(), B.Bytes.begin(), B.Bytes.end());
  }
  Table("Block", "Blocks", "BLOCK_COUNT", Blocks);
  Rows Virtuals, Pages;
  std::vector<Region> VirtualRanges;
  for (const auto &V : S.VirtualAllocations) {
    if (V.Address % ReservationAlignment || !V.Size || V.Size % PageSize ||
        V.Address >= (1ull << 47) || V.Size > (1ull << 47) - V.Address)
      return failure("native virtual reservation is invalid");
    VirtualRanges.push_back({V.Address, V.Size});
    Virtuals.push_back(
        {integer(V.Address), integer(V.Size), integer(V.Protection), "1"});
    uint64_t End = V.Address;
    for (const auto &P : V.Committed) {
      if (P.Address < End || P.Address % PageSize || P.Bytes.empty() ||
          P.Bytes.size() % PageSize || P.Address > V.Address + V.Size ||
          P.Bytes.size() > V.Address + V.Size - P.Address ||
          P.Bytes.size() > MaxBacking - Payload.size() ||
          (P.Permissions & emulation::GuestPermissionMask) != P.Permissions ||
          !(P.Permissions & emulation::UserAccessible))
        return failure("native virtual page ownership is invalid");
      const unsigned Access = P.Permissions;
      const uint32_t Protection = (Access & emulation::Execute)
                                      ? ((Access & emulation::Write)  ? 0x40
                                         : (Access & emulation::Read) ? 0x20
                                                                      : 0x10)
                                      : ((Access & emulation::Write)  ? 4
                                         : (Access & emulation::Read) ? 2
                                                                      : 1);
      Pages.push_back({integer(P.Address), integer(P.Bytes.size()),
                       integer(Payload.size()), integer(Protection)});
      Payload.insert(Payload.end(), P.Bytes.begin(), P.Bytes.end());
      End = P.Address + P.Bytes.size();
    }
  }
  Table("Virtual", "Virtuals", "VIRTUAL_COUNT", Virtuals);
  Table("Page", "Pages", "PAGE_COUNT", Pages);
  O << "static const U8 Payload[]={\n";
  if (Payload.empty())
    O << '0';
  for (size_t I = 0; I < Payload.size(); ++I)
    O << unsigned(Payload[I]) << ',' << ((I & 31) == 31 ? "\n" : "");
  O << "};\n";
  std::set<std::string> ModuleNames;
  Rows Providers;
  for (const auto &M : C.Modules) {
    if (M.Main || M.Base == C.Base)
      continue;
    if (!M.Modeled || !validName(M.Name))
      return failure(
          "native materialization cannot restore another guest DLL's state");
    ModuleNames.insert(llvm::StringRef(M.Name).lower());
    Providers.push_back({integer(M.Base), "0", string(M.Name)});
  }
  Table("Provider", "Providers", "PROVIDER_COUNT", Providers);
  Rows Gates, Slots;
  std::map<std::pair<std::string, std::string>, uint64_t> Identities;
  PreviousEnd = 0;
  for (const auto &[Address, E] : C.Exports) {
    if (Address >= C.Base && Address - C.Base < Input.extent())
      continue;
    if (!ModuleNames.contains(llvm::StringRef(E.Module).lower()) ||
        !validName(E.Module) || !validName(E.Name) || Address < PreviousEnd ||
        Address > UINT64_MAX - 14)
      return failure("native exported-call bindings have unknown or "
                     "overlapping ownership");
    PreviousEnd = Address + 14;
    Identities.emplace(std::pair{llvm::StringRef(E.Module).lower(), E.Name},
                       Address);
    Gates.push_back({integer(Address),
                     string(llvm::StringRef(E.Module).lower()), string(E.Name),
                     "0"});
    GateRanges.push_back({Address, 14});
  }
  Table("Gate", "Gates", "GATE_COUNT", Gates);
  for (const auto &I : Rebuilt.Imports) {
    auto Found = Identities.find({llvm::StringRef(I.Module).lower(), I.Name});
    if (Found == Identities.end())
      return failure(
          "native import cell has no captured exported-call identity");
    Slots.push_back({integer(C.Base + I.SlotRVA), integer(Found->second)});
  }
  Table("Slot", "Slots", "SLOT_COUNT", Slots);
  auto HeapRegions = regions(BlocksRanges), ExportRegions = regions(GateRanges);
  if (!HeapRegions)
    return HeapRegions.takeError();
  if (!ExportRegions)
    return ExportRegions.takeError();
  std::vector<Region> All = *HeapRegions;
  All.insert(All.end(), ExportRegions->begin(), ExportRegions->end());
  auto VirtualRegions = regions(VirtualRanges);
  if (!VirtualRegions)
    return VirtualRegions.takeError();
  All.insert(All.end(), VirtualRegions->begin(), VirtualRegions->end());
  // Reserve space for the compiled runtime as well as the source image.
  All.push_back({C.Base, RuntimeRVA + MaxBacking});
  llvm::sort(
      All, [](const auto &A, const auto &B) { return A.Address < B.Address; });
  PreviousEnd = 0;
  for (const auto &R : All) {
    if (R.Address < PreviousEnd)
      return failure("native state reservations overlap");
    PreviousEnd = R.Address + R.Size;
  }
  auto RegionTable = [&](llvm::StringRef Name, llvm::StringRef Count,
                         const auto &Input) {
    Rows Items;
    for (const auto &R : Input)
      Items.push_back({integer(R.Address), integer(R.Size)});
    Table("Region", Name, Count, Items);
  };
  RegionTable("Regions", "REGION_COUNT", *HeapRegions);
  RegionTable("GateRegions", "GATE_REGION_COUNT", *ExportRegions);
  Rows Criticals;
  auto Owned = [&](uint64_t Address, uint64_t Size, bool Code = false) {
    if (Address >= C.Base && Address - C.Base <= C.Memory.size() &&
        Size <= C.Memory.size() - (Address - C.Base)) {
      const auto *R = Input.regionAt(Address - C.Base);
      return R && (!Code || (C.PageAccess[(Address - C.Base) / PageSize] &
                             emulation::Execute));
    }
    for (const auto &V : S.VirtualAllocations)
      for (const auto &P : V.Committed)
        if (Address >= P.Address && Address - P.Address < P.Bytes.size() &&
            Size <= P.Bytes.size() - (Address - P.Address) &&
            (!Code || (P.Permissions & emulation::Execute)))
          return true;
    if (Code)
      return false;
    return llvm::any_of(S.Allocations, [&](const auto &B) {
      return Address >= B.Address && Address - B.Address <= B.Bytes.size() &&
             Size <= B.Bytes.size() - (Address - B.Address);
    });
  };
  for (const auto &Lock : S.CriticalSections) {
    if (!Owned(Lock.Address, 40) || Lock.Recursion > 1048576)
      return failure(
          "native critical section has unsupported backing or recursion");
    Criticals.push_back({integer(Lock.Address), integer(Lock.Recursion)});
  }
  Table("Critical", "Criticals", "CRITICAL_COUNT", Criticals);
  Rows Fibers;
  for (unsigned I = 0; I < 64; ++I) {
    const auto It = llvm::find_if(S.FiberSlots,
                                  [&](const auto &F) { return F.Index == I; });
    if (It == S.FiberSlots.end()) {
      Fibers.push_back({integer(I), "0", "0", "0", "0", "0"});
      continue;
    }
    if (It->Callback && !Owned(It->Callback, 1, true))
      return failure(
          "native FLS callback is outside the restored executable image");
    Fibers.push_back(
        {integer(I), "1", integer(It->Callback), integer(It->Value), "0", "0"});
  }
  for (const auto &F : S.FiberSlots)
    if (F.Index >= 64)
      return failure("native FLS index exceeds the profile capacity");
  Table("Fiber", "Fibers", "FIBER_CAPACITY", Fibers);
  auto Code = compile(Header, C.Base + RuntimeRVA);
  if (!Code)
    return Code.takeError();
  if (Code->size() > MaxBacking)
    return failure("compiled native runtime exceeds the materialization limit");
  return pe::appendRuntime(std::move(Rebuilt), *Code);
}
} // namespace neverd::unpack::windows
