//===- DarwinLibSystem.h - Modeled libSystem call boundaries -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Exact imported function identities and private process call gates.
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINLIBSYSTEM_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINLIBSYSTEM_H
#include "DarwinProcess.h"

#include "neverd/emulation/AddressSpace.h"

namespace neverd::emulation::darwin_model {
class DarwinCallbacks;
struct LibSystemBinding {
  llvm::StringLiteral Symbol;
  unsigned Arguments;
  std::optional<ServiceKind> Kernel;
  llvm::StringLiteral Provider;
};
std::optional<LibSystemGateKind>
libSystemImport(llvm::StringRef Symbol,
                llvm::StringRef Provider = value::LibSystemPath);
bool isGlobalCallbackRegistration(LibSystemGateKind Kind);
const LibSystemBinding &libSystemBinding(LibSystemGateKind Kind);
struct LibSystemArguments {
  std::array<uint64_t, 6> Carriers{}, Kernel{};
  unsigned Count = 0;
  std::optional<uint32_t> Error;
};
llvm::Expected<LibSystemArguments>
readLibSystemArguments(ExecutionBackend &CPU, const IntegerABI &ABI,
                       uint64_t SP, LibSystemGateKind Kind);
llvm::Error prepareLibSystem(AddressSpace &Space, const ProcessImage &Image);
llvm::Expected<bool>
invokeLibSystem(ExecutionBackend &CPU, DarwinMemory &Memory, DarwinFiles &Files,
                DarwinEntropy &Entropy, const ProcessImage &Image,
                LibSystemGateKind Binding, const ServiceRequest &Request,
                const ProcessOptions &Options, ProcessResult &Result,
                DarwinCallbacks *Callbacks = nullptr);
} // namespace neverd::emulation::darwin_model
#endif
