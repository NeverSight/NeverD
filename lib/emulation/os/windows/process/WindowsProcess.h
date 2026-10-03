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
#include "neverd/emulation/ProcessSession.h"
#include "neverd/loader/COFF/PEProgramExports.h"

#include "llvm/ADT/StringRef.h"

#include <bitset>
#include <map>

namespace neverd::emulation::windows_process {
namespace value {
#define NEVERD_WINDOWS_PROCESS_VALUE(Name, Value)                              \
  inline constexpr uint64_t Name = Value;
#define NEVERD_WINDOWS_PROCESS_BYTES(Name, ...)                                \
  inline constexpr uint8_t Name[] = {__VA_ARGS__};
#include "WindowsProcess.def"
#include "WindowsProcessModules.def"
#undef NEVERD_WINDOWS_PROCESS_BYTES
#undef NEVERD_WINDOWS_PROCESS_VALUE
} // namespace value
namespace text {
#define NEVERD_WINDOWS_PROCESS_TEXT(Name, Text)                                \
  inline constexpr char Name[] = Text;
#include "WindowsProcess.def"
#include "WindowsProcessModules.def"
#undef NEVERD_WINDOWS_PROCESS_TEXT
} // namespace text
inline llvm::Error failure(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 text::Prefix + Message);
}
enum class API {
#define NEVERD_WINDOWS_PROCESS_API(Name, Provider, Count, Returns) Name,
#include "WindowsProcessServices.def"
#undef NEVERD_WINDOWS_PROCESS_API
};
enum class APIProvider { Kernel, Native };
struct Service {
  API Kind;
  const char *Name;
  unsigned Arguments;
  APIProvider Provider;
  bool Returns;
};
llvm::ArrayRef<Service> services();
std::optional<APIProvider> findProvider(llvm::StringRef Module);
const Service *findService(llvm::StringRef Module, llvm::StringRef Name);
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
                                       ImageReadBudget &Budget, bool DLL);
llvm::Error relocateImage(Image &Image, uint64_t Base, ImageReadBudget &Budget);
llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         const ProcessOptions &Options);

struct Program;
struct LoaderRequest {
  enum class Kind { Load, Free, Export };
  Kind Operation;
  uint64_t Module = 0;
  std::string Name;
  std::optional<uint16_t> Ordinal;
};
struct ServiceOutcome {
  std::optional<uint64_t> Value;
  std::optional<LoaderRequest> Request;
  explicit ServiceOutcome(std::optional<uint64_t> Value) : Value(Value) {}
  explicit ServiceOutcome(LoaderRequest Request)
      : Request(std::move(Request)) {}
};
class Services final {
public:
  Services(ExecutionBackend &CPU, AddressSpace &Memory, const Image &Image,
           const Environment &Environment, const ProcessOptions &Options,
           ProcessResult &Result, VirtualMemory &Virtual, Program &Program,
           const ExecutionBudget &Budget)
      : CPU(CPU), Memory(Memory), Loaded(Image), Env(Environment),
        Options(Options), Result(Result), Virtual(Virtual), Modules(Program),
        Budget(Budget) {}
  llvm::Expected<ServiceOutcome> invoke(const Service &Service,
                                        const NativeCallEvent &Event);

private:
  std::optional<uint64_t> unsupported(const Service &Service);
  llvm::Expected<uint64_t> error(uint32_t Code, uint64_t ReturnValue = 0);
  llvm::Expected<bool> access(uint64_t Address, uint64_t Size, unsigned Rights);
  llvm::Expected<std::optional<uint64_t>> heap(const Service &,
                                               const NativeCallEvent &);
  llvm::Expected<std::optional<uint64_t>> memory(const Service &,
                                                 const NativeCallEvent &);
  ExecutionBackend &CPU;
  AddressSpace &Memory;
  const Image &Loaded;
  const Environment &Env;
  const ProcessOptions &Options;
  ProcessResult &Result;
  VirtualMemory &Virtual;
  Program &Modules;
  const ExecutionBudget &Budget;
  std::bitset<value::DynamicTLSCount> TLSSlots;
  struct Allocation {
    uint64_t Size, MappedSize;
  };
  std::map<uint64_t, Allocation> Allocations;
};
} // namespace neverd::emulation::windows_process
#endif
