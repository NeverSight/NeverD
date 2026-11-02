//===- ProcessImports.h - Observe import helpers -----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_DYNAMIC_PROCESSIMPORTS_H
#define NEVERD_UNPACK_DYNAMIC_PROCESSIMPORTS_H

#include "../arch/x86_64/X64Imports.h"
#include "../core/Capture.h"

#include "neverd/emulation/ProcessObserver.h"

#include <array>

namespace neverd::unpack {
/// Collects actual export boundaries, including the last opaque export that
/// stops execution. No API signature, result or guest effect is fabricated.
class ExportObserver final : public emulation::ProcessObserver {
public:
  struct Call {
    uint64_t ReturnAddress, Gate;
  };
  llvm::Expected<std::vector<emulation::ExecutionWatch>>
  started(emulation::ProcessView &Process) override;
  llvm::Expected<std::optional<std::vector<emulation::ExecutionWatch>>>
  watched(emulation::ProcessView &Process, uint64_t PC) override;
  llvm::Expected<std::optional<std::vector<emulation::ExecutionWatch>>>
  invoking(emulation::ProcessView &Process) override;
  llvm::Error exporting(emulation::ProcessView &,
                        const emulation::ProcessExportView &Export,
                        std::optional<uint64_t> ReturnAddress) override;
  llvm::ArrayRef<Call> calls() const { return Calls; }

private:
  std::vector<Call> Calls;
  std::vector<emulation::ExecutionWatch> Watches;
};

/// Watches bounded helpers near previously observed export continuations.
/// A result needs a balanced frame, unchanged non-result registers and no
/// persistent RAM or mapping effects. Callee stack scratch is excluded by
/// the Win64 ABI; caller-owned stack bytes remain part of the comparison.
class ImportObserver final : public emulation::ProcessObserver {
public:
  ImportObserver(llvm::ArrayRef<uint64_t> Continuations,
                 llvm::ArrayRef<uint64_t> Gates)
      : Continuations(Continuations.begin(), Continuations.end()),
        Gates(Gates.begin(), Gates.end()) {}
  llvm::Expected<std::vector<emulation::ExecutionWatch>>
  started(emulation::ProcessView &Process) override;
  llvm::Expected<std::optional<std::vector<emulation::ExecutionWatch>>>
  watched(emulation::ProcessView &Process, uint64_t PC) override;
  llvm::Expected<std::optional<std::vector<emulation::ExecutionWatch>>>
  invoking(emulation::ProcessView &Process) override;
  std::vector<TailImport> takeImports();

private:
  struct Candidate {
    uint64_t RVA;
    unsigned Capacity, CallOffset;
    std::array<uint8_t, x64::MaxAddressLoadSize> Bytes{};
    std::optional<x64::ImportSite> Proven;
    bool ExportContinuation = false;
    bool Rejected = false;
  };
  struct Region {
    uint64_t Address;
    std::vector<uint8_t> Bytes;
  };
  struct Pending {
    size_t Candidate;
    uint64_t NativeCalls;
    std::vector<emulation::RegisterValue> Registers;
    std::vector<emulation::AddressMapping> Layout;
    std::vector<Region> Memory;
  };
  llvm::Expected<std::vector<emulation::RegisterValue>>
  registers(emulation::ProcessView &Process);
  llvm::Expected<bool> unchanged(emulation::ProcessView &Process,
                                 const Pending &Before);
  void reject(size_t Candidate);
  void rejectActive();
  llvm::Error complete(emulation::ProcessView &Process, uint64_t PC);
  uint64_t Base = 0;
  bool Initialized = false;
  std::vector<uint64_t> Continuations, Gates;
  std::vector<uint8_t> Code;
  std::vector<Candidate> Candidates;
  std::vector<emulation::ExecutionWatch> Watches;
  std::optional<Pending> Active;
  std::vector<TailImport> Imports;
};
} // namespace neverd::unpack
#endif
