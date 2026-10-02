//===- AndroidInternal.h - Android native environment boundaries -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ANDROIDINTERNAL_H
#define NEVERD_EMULATION_ANDROIDINTERNAL_H
#include "../LinuxMemory.h"

#include "neverd/emulation/AndroidNative.h"
#include "neverd/loader/ELF/ELFProgramLinking.h"

#include <map>

namespace neverd::emulation::android_model {
inline constexpr uint64_t PageSize = 4096;
inline constexpr uint64_t TLSAddress = 0x7000000000;
inline constexpr uint64_t StdioAddress = TLSAddress - PageSize;
inline constexpr uint64_t ThunkBase = TLSAddress + PageSize;
inline constexpr uint64_t ErrnoAddress = TLSAddress + 2 * 8;
inline constexpr uint64_t GuardAddress = TLSAddress + 5 * 8;
inline constexpr uint64_t LinkerErrorAddress = TLSAddress + 0x200;
inline constexpr uint64_t LinkerErrorSlot = TLSAddress + 6 * 8;
inline constexpr uint64_t StackGuard = 0x91ab62e5347c2800;
inline constexpr uint16_t ModelTrap = 0x4e44;
inline constexpr uint64_t ReturnPC = ThunkBase;
inline llvm::Error failure(llvm::Twine Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "Android native: " + Message);
}
struct LinkedImage {
  uint64_t Entry, InitialBreak;
  std::vector<uint64_t> Constructors;
  std::map<uint64_t, std::string> Imports;
  std::map<uint64_t, std::string> DynamicProviders;
  std::map<std::string, std::map<std::string, uint64_t>> Libraries;
};
llvm::Expected<LinkedImage> loadImage(AddressSpace &Space,
                                      const BinaryImage &Image,
                                      const ProcessOptions &Options);
class Bionic {
public:
  Bionic(ExecutionBackend &CPU, linux_model::LinuxMemory &Memory,
         const linux_model::ProcessLayout &Layout,
         const ProcessOptions &Options, ProcessResult &Result,
         ExecutionBudget &Budget, const LinkedImage &Linked)
      : CPU(CPU), Memory(Memory), Layout(Layout), Options(Options),
        Result(Result), Budget(Budget), Linked(Linked) {}
  bool timedOut() const { return Expired; }
  llvm::Expected<std::optional<uint64_t>> invoke(NativeCallEvent &Call);

private:
  ExecutionBackend &CPU;
  linux_model::LinuxMemory &Memory;
  const linux_model::ProcessLayout &Layout;
  const ProcessOptions &Options;
  ProcessResult &Result;
  ExecutionBudget &Budget;
  const LinkedImage &Linked;
  bool Expired = false;
  struct LibraryState {
    uint64_t Handle = 0, References = 0;
  };
  std::map<std::string, LibraryState> OpenLibraries;
  std::map<uint64_t, std::string> Handles;
  uint64_t NextHandle = 1;
  struct Allocation {
    uint64_t Size, MappedSize;
  };
  std::map<uint64_t, Allocation> Allocations;
  llvm::Error access(uint64_t Address, uint64_t Size, unsigned Permissions);
  llvm::Expected<uint8_t> byte(uint64_t Address);
  llvm::Expected<std::string> string(uint64_t Address);
  llvm::Expected<uint64_t> allocate(uint64_t Size);
  llvm::Error release(uint64_t Address);
  llvm::Error setErrno(uint32_t Value);
  llvm::Expected<uint64_t> linkerError(llvm::StringRef Message,
                                       uint64_t ReturnValue = 0);
  llvm::Expected<std::optional<uint64_t>> dlfcn(NativeCallEvent &Call);
};
llvm::Expected<ProcessResult> runNative(const std::filesystem::path &Path,
                                        const ProcessOptions &Options);
} // namespace neverd::emulation::android_model
#endif
