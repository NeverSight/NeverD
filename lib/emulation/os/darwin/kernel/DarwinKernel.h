//===- DarwinKernel.h - Shared Darwin kernel contracts ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINKERNEL_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINKERNEL_H

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ProcessSession.h"

namespace neverd::emulation::darwin_model {
namespace value {
#define NEVERD_DARWIN_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#define NEVERD_DARWIN_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "../DarwinValues.def"
#undef NEVERD_DARWIN_TEXT
#undef NEVERD_DARWIN_VALUE
} // namespace value
namespace diagnostic {
#define NEVERD_DARWIN_DIAGNOSTIC(Name, Text)                                   \
  inline constexpr char Name[] = Text;
#include "../DarwinValues.def"
#undef NEVERD_DARWIN_DIAGNOSTIC
} // namespace diagnostic
inline llvm::Error failure(llvm::StringRef Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
struct MaximumProtection {
  uint64_t Address, Size;
  unsigned Protection;
};
/// Guest page policy and maximum protections established by the image loader.
struct MemoryLayout {
  uint64_t PageSize, MinimumAddress;
  std::vector<MaximumProtection> Maximum;
};
enum class ServiceConvention { BSD, Mach };
struct ServiceResult {
  uint64_t Value;
  bool Error = false;
  /// The resolved binding owns the return convention and report semantics.
  ServiceConvention Convention = ServiceConvention::BSD;
};
enum class ServiceKind {
#define NEVERD_DARWIN_SERVICE(Name, Number, ReturnType) Name,
#define NEVERD_DARWIN_MACH_SERVICE(Name, Number, ARM64Only) Name,
#include "../DarwinValues.def"
#undef NEVERD_DARWIN_MACH_SERVICE
#undef NEVERD_DARWIN_SERVICE
};
class DarwinMemory;
class DarwinFiles;
class DarwinEntropy;
llvm::Expected<ProcessServiceEvent> readService(ExecutionBackend &CPU,
                                                const ServiceRequest &Request);
llvm::Error returnService(ExecutionBackend &CPU, const ServiceRequest &Request,
                          ServiceResult Result);
llvm::Expected<std::optional<ServiceResult>>
handleService(ExecutionBackend &CPU, DarwinMemory &Memory, DarwinFiles &Files,
              DarwinEntropy &Entropy, const ProcessServiceEvent &Event,
              const ProcessOptions &Options, ProcessResult &Result);
} // namespace neverd::emulation::darwin_model
#endif
