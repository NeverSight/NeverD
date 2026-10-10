//===- WindowsProcess.h - Windows user process boundaries ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_PROCESS_H
#define NEVERD_EMULATION_WINDOWS_PROCESS_H
#include "WindowsProcessMemory.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/IntegerABI.h"
#include "neverd/emulation/ProcessObserver.h"
#include "neverd/emulation/ProcessSession.h"
#include "neverd/emulation/WindowsProcessState.h"
#include "neverd/loader/COFF/PEProgramExports.h"
#include "neverd/loader/ExceptionTable.h"

#include "llvm/ADT/StringRef.h"

#include <bitset>
#include <map>
#include <set>

namespace neverd::emulation::windows_process {
namespace value {
#define NEVERD_WINDOWS_PROCESS_VALUE(Name, Value)                              \
  inline constexpr uint64_t Name = Value;
#define NEVERD_WINDOWS_PROCESS_BYTES(Name, ...)                                \
  inline constexpr uint8_t Name[] = {__VA_ARGS__};
#include "WindowsContextCapture.def"
#include "WindowsLibraryHost.def"
#include "WindowsProcess.def"
#include "WindowsProcessExceptions.def"
#include "WindowsProcessModules.def"
#include "WindowsSystemModules.def"
#undef NEVERD_WINDOWS_PROCESS_BYTES
#undef NEVERD_WINDOWS_PROCESS_VALUE
} // namespace value
namespace text {
#define NEVERD_WINDOWS_PROCESS_TEXT(Name, Text)                                \
  inline constexpr char Name[] = Text;
#include "WindowsContextCapture.def"
#include "WindowsLibraryHost.def"
#include "WindowsProcess.def"
#include "WindowsProcessExceptions.def"
#include "WindowsProcessModules.def"
#include "WindowsSystemModules.def"
#undef NEVERD_WINDOWS_PROCESS_TEXT
} // namespace text
inline llvm::Error failure(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 text::Prefix + Message);
}
enum class API {
#define NEVERD_WINDOWS_PROCESS_API(Name, Provider, Count, Returns) Name,
#define NEVERD_WINDOWS_PROCESS_NAMED_API(Name, Symbol, Provider, Count,        \
                                         Returns)                              \
  Name,
#include "WindowsProcessServices.def"
#undef NEVERD_WINDOWS_PROCESS_NAMED_API
#undef NEVERD_WINDOWS_PROCESS_API
};
enum class APIProvider { Kernel, Native };
struct SystemProvider {
  const char *Name;
  APIProvider Family;
  uint64_t Base;
};
llvm::ArrayRef<SystemProvider> systemProviders();
struct Service {
  API Kind;
  const char *Name;
  unsigned Arguments;
  APIProvider Provider;
  bool Returns;
};
struct NativeService {
  const char *Name;
  uint32_t Number;
};
llvm::ArrayRef<NativeService> nativeServices();
std::optional<uint32_t> nativeServiceNumber(llvm::StringRef Name);
const NativeService *findNativeService(uint64_t Number);
/// The model's x64 native boundary uses the Win64 stub stack layout, with
/// argument zero in R10. Unknown numbers never select a named API.
llvm::Expected<uint64_t> readNativeServiceArgument(ExecutionBackend &CPU,
                                                   uint64_t SP, unsigned Index);
llvm::Error returnNativeService(ExecutionBackend &CPU,
                                const ServiceRequest &Request, uint64_t Result);
llvm::ArrayRef<Service> services();
std::optional<APIProvider> findProvider(llvm::StringRef Module);
const Service *findService(llvm::StringRef Module, llvm::StringRef Name);
bool isServiceAbsent(llvm::StringRef Module, llvm::StringRef Name);
struct ImageRegion {
  uint64_t Address;
  unsigned Permissions;
  std::vector<uint8_t> Bytes;
  uint64_t ContentSize = 0, FileSize = 0;
};
struct Import {
  uint64_t Slot;
  const Service *Target;
  std::string Module;
  uint64_t Gate;
  std::string Name;
  std::optional<uint16_t> Ordinal;
};
struct Image {
  GuestArchitecture Architecture;
  uint64_t Base, Size, Entry;
  std::vector<ImageRegion> Regions;
  std::vector<Import> Imports;
  uint64_t TLSIndex = 0, TLSSize = 0, TLSCallbackPointer = 0;
  uint64_t TLSDirectory = 0, TLSTemplate = 0, TLSTemplateSize = 0;
  uint64_t TLSAlignment = value::PointerSize;
  std::vector<std::string> Dependencies;
  std::vector<uint64_t> Relocations;
  PEProgramExports Exports;
  bool Relocatable = false;
  bool DLL = false;
  uint64_t PreferredBase = 0;
  std::shared_ptr<const ExceptionInfo> Exceptions;
  std::vector<PEMetadataRange> ExceptionMetadata;
  /// The exception directory was not file backed at load, so no frame of
  /// this image can be unwound from loader-owned metadata.
  bool ExceptionsDeferred = false;
};
struct ModuleIdentity {
  std::string Name;
  uint64_t Base, Size, Entry;
};
struct ImageReadBudget {
  uint64_t FileBytes, MappedBytes;
  uint64_t Records = windows_process_limits::MetadataRecords;
  uint64_t MetadataBytes = windows_process_limits::MetadataBytes;
};
struct Environment {
  uint64_t CommandLine;
  uint64_t Variables = 0;
  std::u16string ImageName;
  uint64_t StringCursor = 0;
  std::map<size_t, uint64_t> ModuleNames;
  struct TLSAllocation {
    uint64_t Index, Address, Size;
  };
  std::map<size_t, TLSAllocation> TLS;
  std::map<uint64_t, std::vector<uint8_t>> LoaderMetadata;
};
llvm::Expected<Image> loadImage(const std::filesystem::path &Path,
                                uint64_t MemoryLimit);
llvm::Expected<Image> loadProgramImage(const std::filesystem::path &Path,
                                       ImageReadBudget &Budget,
                                       std::optional<bool> DLL,
                                       bool DeferUnmodeled = false);
llvm::Error relocateImage(Image &Image, uint64_t Base, ImageReadBudget &Budget);
/// A null observer runs without caller observation. The model may still
/// watch native entry prologues to establish export-call provenance.
llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         const ProcessOptions &Options,
                                         ProcessObserver *Observer = nullptr);

struct Program;
struct LoaderRequest {
  enum class Kind { Load, Free, Export };
  Kind Operation;
  uint64_t Module = 0;
  std::string Name;
  std::optional<uint16_t> Ordinal;
};
struct FLSCleanup {
  uint32_t Index;
  uint64_t Function, Argument;
  bool ReleaseIndex = true;
};
struct ServiceOutcome {
  struct Exception {
    uint32_t Code, Flags;
    uint64_t Address;
    std::vector<uint64_t> Arguments;
  };
  std::optional<uint64_t> Value;
  std::optional<LoaderRequest> Request;
  std::optional<Exception> Raised;
  std::optional<FLSCleanup> Cleanup;
  explicit ServiceOutcome(std::optional<uint64_t> Value) : Value(Value) {}
  explicit ServiceOutcome(LoaderRequest Request)
      : Request(std::move(Request)) {}
  explicit ServiceOutcome(Exception Raised) : Raised(std::move(Raised)) {}
  explicit ServiceOutcome(FLSCleanup Cleanup) : Cleanup(Cleanup) {}
};
class ExceptionDispatcher;
class Services final {
public:
  Services(ExecutionBackend &CPU, AddressSpace &Memory, const Image &Image,
           const Environment &Environment, const ProcessOptions &Options,
           ProcessResult &Result, VirtualMemory &Virtual, Program &Program,
           const ExecutionBudget &Budget, ExceptionDispatcher &Exceptions)
      : CPU(CPU), Memory(Memory), Loaded(Image), Env(Environment),
        Options(Options), Result(Result), Virtual(Virtual), Modules(Program),
        Budget(Budget), Exceptions(Exceptions) {}
  llvm::Expected<ServiceOutcome> invoke(const Service &Service,
                                        const NativeCallEvent &Event);
  llvm::Expected<bool> complete(FLSCleanup &Cleanup);
  llvm::Expected<std::optional<FLSCleanup>> exitCleanup();
  std::vector<ProcessHeapAllocationView> heapAllocations() const;
  std::vector<uint64_t> encodedPointers() const {
    return {EncodedPointers.begin(), EncodedPointers.end()};
  }
  llvm::Expected<ProcessDynamicThreadLocalState>
  dynamicThreadLocalState() const;
  llvm::Expected<std::shared_ptr<const ProcessRuntimeState>>
  runtimeState(bool IncludeBacking) const;

private:
  std::optional<uint64_t> unsupported(const Service &Service);
  llvm::Expected<uint64_t> error(uint32_t Code, uint64_t ReturnValue = 0);
  llvm::Expected<bool> access(uint64_t Address, uint64_t Size, unsigned Rights);
  llvm::Expected<std::optional<uint64_t>> heap(const Service &,
                                               const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> openImage(const Service &,
                                                    const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>>
  createSection(const Service &, const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> openSection(const Service &,
                                                      const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> mapSection(const Service &,
                                                     const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> unmapSection(const Service &,
                                                       const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> closeHandle(const Service &,
                                                      const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>>
  protectMemory(const Service &, const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> querySystem(const Service &,
                                                      const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> queryProcess(const Service &,
                                                       const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> queryThread(const Service &,
                                                      const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> setThread(const Service &,
                                                    const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> delay(const Service &,
                                                const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> clock(const Service &,
                                                const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>>
  encodePointer(const Service &, const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>>
  criticalSection(const Service &, const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> crt(const Service &,
                                              const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> toolhelp(const Service &,
                                                   const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> environment(const Service &,
                                                      const NativeCallEvent &);
  llvm::Expected<std::u16string> readWide(uint64_t Address, uint64_t Limit);
  llvm::Error writeWide(uint64_t Address, const std::u16string &Text);
  llvm::Expected<std::optional<uint64_t>> memory(const Service &,
                                                 const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>>
  writeProcessMemory(const Service &, const NativeCallEvent &);
  ExecutionBackend &CPU;
  AddressSpace &Memory;
  const Image &Loaded;
  const Environment &Env;
  const ProcessOptions &Options;
  ProcessResult &Result;
  VirtualMemory &Virtual;
  Program &Modules;
  const ExecutionBudget &Budget;
  ExceptionDispatcher &Exceptions;
  std::bitset<value::DynamicTLSCount> TLSSlots;
  std::bitset<value::DynamicTLSCount> FLSSlots;
  struct FiberData {
    uint64_t Callback, Value;
    bool Cleaning = false;
  };
  std::map<uint32_t, FiberData> FLSData;
  uint32_t FLSHighIndex = 0;
  struct FiberExit {
    uint32_t Next = 0, Limit;
  };
  std::optional<FiberExit> FLSExit;
  struct Allocation {
    uint64_t Size, MappedSize;
    bool EnvironmentSnapshot = false;
    uint64_t Heap = value::HeapHandle;
  };
  llvm::Expected<uint64_t> allocateHeap(uint64_t Size, bool Snapshot = false,
                                        uint64_t Heap = value::HeapHandle);
  llvm::Expected<bool> mapHeapPages(uint64_t Address, uint64_t Size);
  llvm::Expected<uint64_t> reallocateHeap(uint64_t Address, uint64_t Size,
                                          uint32_t Flags);
  std::map<uint64_t, Allocation> Allocations;
  std::set<uint64_t> CreatedHeaps;
  // At most one distinct value per completed service, bounded by the process
  // event budget. Clearing guest storage does not erase the observed value.
  std::set<uint64_t> EncodedPointers;
  uint64_t NextCreatedHeap = value::CreatedHeapBase;
  uint64_t CommandLineA = 0;
  std::map<uint64_t, bool> ThreadSnapshots;
  uint64_t NextThreadSnapshot = value::ThreadSnapshotBase;
  uint32_t ThreadErrorMode = 0;
  bool ThreadHiddenFromDebugger = false;
  // The process profile has one executing thread. Keep initialization and
  // recursion authoritative instead of accepting fabricated guest fields.
  std::map<uint64_t, uint32_t> CriticalSections;
  bool knownHeap(uint64_t Handle) const {
    if (Handle == value::HeapHandle)
      return true;
    return CreatedHeaps.contains(Handle);
  }
  struct OpenedFile {
    std::filesystem::path Path;
    uint64_t Size = 0;
  };
  struct SectionObject {
    std::filesystem::path Path;
    uint64_t Size = 0;
    std::optional<size_t> SystemModule;
  };
  struct MappedView {
    uint64_t Size = 0;
    bool Image = false;
  };
  std::map<uint64_t, OpenedFile> Files;
  std::map<uint64_t, SectionObject> Sections;
  std::map<uint64_t, MappedView> Views;
};
} // namespace neverd::emulation::windows_process
#endif
