//===- ExecutionDiagnostics.h - Execution errors -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_EXECUTIONDIAGNOSTICS_H
#define NEVERD_EMULATION_CORE_EXECUTIONDIAGNOSTICS_H
#include "llvm/Support/Error.h"

namespace neverd::emulation {
class BackendUnavailableError
    : public llvm::ErrorInfo<BackendUnavailableError> {
public:
  static char ID;
  explicit BackendUnavailableError(const char *Reason) : Reason(Reason) {}
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }

private:
  const char *Reason;
};

class UnsupportedExecutionError
    : public llvm::ErrorInfo<UnsupportedExecutionError> {
public:
  static char ID;
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }
};
namespace diagnostic {
#define NEVERD_EXECUTION_DIAGNOSTIC(Name, Text)                                \
  inline constexpr char Name[] = Text;
#include "ExecutionDiagnostics.def"
#undef NEVERD_EXECUTION_DIAGNOSTIC
inline llvm::Error error(const char *Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
inline llvm::Error unavailable(const char *Text) {
  return llvm::make_error<BackendUnavailableError>(Text);
}
} // namespace diagnostic
} // namespace neverd::emulation
#endif
