//===- DarwinProcess.h - Shared Darwin process contracts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINPROCESS_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINPROCESS_H

#include "../kernel/DarwinKernel.h"

#include "neverd/emulation/ImageMapping.h"
#include "neverd/emulation/IntegerABI.h"
#include "neverd/loader/MachO/MachOProgramLinking.h"

#include <map>

namespace neverd::emulation::darwin_model {
/// Caller-owned admission stop; ordinary image/CPU errors keep their types.
class PreparationTimeout : public llvm::ErrorInfo<PreparationTimeout> {
public:
  static char ID;
  GuestArchitecture Architecture;
  uint64_t Entry;
  PreparationTimeout(GuestArchitecture Architecture, uint64_t Entry)
      : Architecture(Architecture), Entry(Entry) {}
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }
};
struct ProfileSpec {
  ProcessProfile Profile;
  uint32_t Platform;
  bool AllowX64;
};
ProfileSpec macOSProfile();
ProfileSpec iOSProfile(bool Simulator);
enum class LibSystemGateKind {
#define NEVERD_DARWIN_LIBSYSTEM_CALL(Name, Symbol, Count, Kernel) Name,
#include "DarwinLibSystemCalls.def"
#undef NEVERD_DARWIN_LIBSYSTEM_CALL
  StubBinder,
  TLVBootstrap
};
struct PreparedFixup {
  uint64_t Address, Value;
};
struct TLSImage {
  uint64_t Template, Size, Storage;
};
struct TLSDescriptor {
  size_t Image;
  uint64_t Offset;
};
struct ExecutableSpan {
  uint64_t Address, Size;
};
struct StartupCallback {
  uint64_t Slot, Word, Target;
  unsigned Width;
};
struct ProcessImage {
  GuestArchitecture Architecture;
  MemoryLayout Memory;
  bool MainEntry;
  ImageMappingPlan Plan;
  std::vector<PreparedFixup> Fixups;
  std::map<uint64_t, LibSystemGateKind> Imports;
  uint64_t MetadataBytes = 0;
  std::vector<TLSImage> TLS;
  std::map<uint64_t, TLSDescriptor> TLSDescriptors;
  std::vector<ExecutableSpan> ExecutableSpans;
  std::vector<StartupCallback> Initializers;
  bool HasTLSCallbacks = false;
  bool HasGlobalCallbacks = false;
};
struct InitialStack {
  uint64_t SP, Argc, Argv, Envp, Apple, ProgramVars = 0;
};
llvm::Expected<ProcessImage> loadImage(const std::filesystem::path &Path,
                                       ProfileSpec Profile,
                                       const ProcessOptions &Options,
                                       ExecutionBudget *Budget = nullptr);
llvm::Expected<InitialStack> prepareStack(GuestMemory &Memory,
                                          const ProcessOptions &Options,
                                          llvm::StringRef ExecutableName,
                                          uint64_t MainHeader = 0);
llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         ProfileSpec Profile,
                                         const ProcessOptions &Options);
} // namespace neverd::emulation::darwin_model
#endif
