//===- WindowsProcessLifetime.h - Module lifetimes ----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_PROCESS_LIFETIME_H
#define NEVERD_EMULATION_WINDOWS_PROCESS_LIFETIME_H
#include "WindowsProcessModules.h"

namespace neverd::emulation::windows_process {
class Lifetime final {
public:
  enum class CallKind { TLS, DLL, Entry };
  struct Call {
    CallKind Kind;
    uint64_t PC, ReturnGate;
    std::vector<uint64_t> Arguments;
  };
  enum class Mode { Startup, Load, Unload, Rollback, Exit };
  explicit Lifetime(Program &Program);
  Lifetime(Program &Program, Mode Kind, llvm::ArrayRef<ModuleRef> Order,
           std::optional<ModuleRef> Failed = std::nullopt);
  std::optional<ModuleRef> failedModule() const { return Failed; };
  bool detaching() const {
    return Kind == Mode::Unload || Kind == Mode::Rollback || Kind == Mode::Exit;
  }
  llvm::Expected<std::optional<Call>> next(ExecutionBackend &CPU);
  llvm::Error returned(uint64_t Value);
  llvm::Error exit(uint32_t Status);
  std::optional<uint32_t> exitStatus() const { return ExitStatus; }

private:
  struct Notification {
    ModuleRef Module;
    CallKind Kind;
    uint64_t Reason;
  };
  llvm::Error beginExit(uint32_t Status, bool InitializationFailed);
  void advance();
  Program &Program;
  Mode Kind = Mode::Startup;
  std::optional<ModuleRef> Failed;
  std::vector<Notification> Pending;
  size_t Position = 0, Callback = 0;
  std::optional<uint64_t> CallbackArray;
  bool Running = false;
  std::optional<uint32_t> ExitStatus;
};
} // namespace neverd::emulation::windows_process
#endif
