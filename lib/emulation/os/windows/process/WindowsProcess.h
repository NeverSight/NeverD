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
#undef NEVERD_WINDOWS_PROCESS_BYTES
#undef NEVERD_WINDOWS_PROCESS_VALUE
} // namespace value
namespace text {
#define NEVERD_WINDOWS_PROCESS_TEXT(Name, Text)                                \
  inline constexpr char Name[] = Text;
#include "WindowsProcess.def"
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
};
struct Import {
  uint64_t Slot;
  const Service *Target;
  std::string Module;
  uint64_t Gate;
};
struct Image {
  GuestArchitecture Architecture;
  uint64_t Base, Size, Entry;
  std::vector<ImageRegion> Regions;
  std::vector<Import> Imports;
  uint64_t TLSIndex = 0, TLSSize = 0, TLSCallbackPointer = 0;
  std::vector<uint8_t> TLSBytes;
  std::vector<uint64_t> TLSCallbacks;
};
struct Environment {
  uint64_t CommandLine;
  std::u16string ImageName;
};
llvm::Expected<Image> loadImage(const std::filesystem::path &Path,
                                uint64_t MemoryLimit);
llvm::Expected<Environment> prepareEnvironment(AddressSpace &Memory,
                                               const Image &Image,
                                               const ProcessOptions &Options,
                                               llvm::StringRef ImageName);
llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         const ProcessOptions &Options);

class Services final {
public:
  Services(ExecutionBackend &CPU, AddressSpace &Memory, const Image &Image,
           const Environment &Environment, const ProcessOptions &Options,
           ProcessResult &Result)
      : CPU(CPU), Memory(Memory), Loaded(Image), Env(Environment),
        Options(Options), Result(Result), Virtual(Memory, Image, Options) {}
  llvm::Expected<std::optional<uint64_t>> invoke(const Service &Service,
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
  VirtualMemory Virtual;
  std::bitset<value::DynamicTLSCount> TLSSlots;
  struct Allocation {
    uint64_t Size, MappedSize;
  };
  std::map<uint64_t, Allocation> Allocations;
};
} // namespace neverd::emulation::windows_process
#endif
