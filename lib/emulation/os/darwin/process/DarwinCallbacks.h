//===- DarwinCallbacks.h - Bounded process callback lifecycle ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Private startup, TLS and process-exit callback continuations.
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINCALLBACKS_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINCALLBACKS_H

#include "DarwinProcess.h"

namespace neverd::emulation::darwin_model {
class DarwinCallbacks {
  enum class Phase { Startup, Main, TLS, Global, Complete };
  struct TLSCallback {
    uint64_t Function, Object;
  };
  struct GlobalCallback {
    uint64_t Function, Object;
    bool HasObject;
  };
  using TLSCallbacks = std::vector<TLSCallback>;
  struct CombinedCallbacks {
    TLSCallbacks TLS;
    std::vector<GlobalCallback> Global;
  };
  struct Frame {
    uint64_t Target, ReturnSP;
  };
  const ProcessImage &Image;
  const InitialStack &Stack;
  uint64_t StackBase;
  Phase State = Phase::Startup;
  size_t NextInitializer = 0;
  std::optional<Frame> Active;
  uint64_t MainReturnSP = 0, TerminationTop = 0, ExitStatus = 0;
  std::optional<uint64_t> MainValue;
  // The immutable image fact selects the active member. Keep the old base
  // footprint and TLS record stride for programs without global imports.
  union {
    TLSCallbacks Pending;
    CombinedCallbacks *Combined;
  };

  TLSCallbacks &tls();

  llvm::Error target(ExecutionBackend &CPU, uint64_t Address) const;
  llvm::Error enter(ExecutionBackend &CPU, uint64_t Address, uint64_t Top,
                    llvm::ArrayRef<uint64_t> Arguments, bool Callback);
  llvm::Error startup(ExecutionBackend &CPU);
  llvm::Expected<bool> terminate(ExecutionBackend &CPU, ProcessResult &Result);

public:
  static constexpr size_t TLSCallbackLimit = 1024;
  static constexpr size_t GlobalCallbackLimit = 1024;
  static constexpr uint64_t ReturnGate = value::ReturnGate + 16;
  static uint64_t metadataBytes(bool TLS, bool Global = false) {
    return sizeof(DarwinCallbacks) +
           (TLS ? TLSCallbackLimit * sizeof(TLSCallback) : 0) +
           (Global ? sizeof(CombinedCallbacks) +
                         GlobalCallbackLimit * sizeof(GlobalCallback)
                   : 0);
  }
  DarwinCallbacks(const ProcessImage &Image, const ProcessOptions &Options,
                  const InitialStack &Stack);
  ~DarwinCallbacks();
  DarwinCallbacks(const DarwinCallbacks &) = delete;
  DarwinCallbacks &operator=(const DarwinCallbacks &) = delete;
  llvm::Expected<uint64_t> start(ExecutionBackend &CPU);
  llvm::Error addTLS(ExecutionBackend &CPU, uint64_t Function, uint64_t Object);
  llvm::Error addGlobal(ExecutionBackend &CPU, uint64_t Function,
                        std::optional<uint64_t> Object);
  llvm::Expected<bool> exit(ExecutionBackend &CPU, uint64_t Status,
                            ProcessResult &Result,
                            std::optional<uint64_t> ReturnValue = std::nullopt);
  llvm::Expected<bool> returned(ExecutionBackend &CPU, uint64_t Gate,
                                ProcessResult &Result);
};
} // namespace neverd::emulation::darwin_model
#endif
