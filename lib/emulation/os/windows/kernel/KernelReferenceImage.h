//===- KernelReferenceImage.h - Read-only kernel code observations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read-only kernel code observations.
///

#ifndef NEVERD_EMULATION_KERNELREFERENCEIMAGE_H
#define NEVERD_EMULATION_KERNELREFERENCEIMAGE_H
#include "KernelReferenceExports.h"

#include "neverd/emulation/ImageMapping.h"

#include "llvm/ADT/ArrayRef.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
namespace neverd::modules {
class ModuleImage;
}
namespace neverd::emulation {
class KernelReferenceImage {
public:
  static llvm::Expected<KernelReferenceImage>
  load(const std::filesystem::path &Path, uint64_t MemoryLimit);
  static llvm::Expected<KernelReferenceImage>
  load(const std::filesystem::path &Path, uint64_t MemoryLimit,
       const std::filesystem::path &PDBPath);
  /// Consume one pinned source observation. Optional private layout facts
  /// require its authenticated PDB; other debug kinds grant no kernel facts.
  /// This adapter owns kernel policy, not file/debug matching or source IO.
  static llvm::Expected<KernelReferenceImage>
  fromModule(const modules::ModuleImage &, uint64_t MemoryLimit);
  llvm::Error map(GuestMemory &Memory) const;
  bool contains(uint64_t Address) const;
  bool matchesCode(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes) const;
  uint64_t base() const { return Base; }
  uint64_t size() const { return Size; }
  const std::string &digest() const { return Digest; }
  const std::string &pdbDigest() const { return PDBDigest; }
  const std::map<std::string, uint64_t> &threadInitializationSymbols() const {
    return ThreadInitializationSymbols;
  }
  struct DebuggerFacts {
    std::vector<uint8_t> Template;
    uint64_t Address = 0, ListHead = 0, PagedPoolCommit = 0, LoaderBlock = 0;
    uint64_t SystemVaAssignments = 0;
    uint64_t SecureCookie = 0;
    uint64_t HalCacheDispatch = 0;
    uint64_t GuardBitmapPointer = 0;
    uint64_t ActiveProcessHead = 0;
    uint64_t UnifiedSystemCallEntry = 0;
    uint64_t ServiceDescriptor = 0, ServiceTable = 0, ServiceArguments = 0;
    uint64_t PciMcfgCount = 0, PciMcfgPointer = 0;
    std::vector<uint8_t> CompactedServices, ServiceArgumentBytes;
    uint32_t ActiveProcessLinksOffset = 0, ProcessIDOffset = 0;
    std::vector<uint64_t> GuardTargets;
    std::vector<uint64_t> SuppressedExports;
    uint32_t ProcessSize = 0, ThreadSize = 0;
    uint32_t ThreadIDOffset = 0, ThreadProcessIDOffset = 0;
    uint32_t ThreadProcessOffset = 0, ThreadWin32StartOffset = 0;
    uint32_t ThreadWin32ThreadOffset = 0;
  };
  const std::optional<DebuggerFacts> &debugger() const { return Debugger; }
  const std::vector<ImageMapping> &opaqueData() const { return OpaqueData; }
  const KernelReferenceExports &exportData() const { return ExportData; }
  /// Zero denotes present data or an ambiguous code alias, kept as a trap.
  const std::map<std::string, uint64_t> &exports() const { return Exports; }
  const std::map<std::string, uint64_t> &objectTypeExports() const {
    return ObjectTypeExports;
  }
  const std::map<std::string, uint64_t> &processDataExports() const {
    return ProcessDataExports;
  }
  const std::map<std::string, uint64_t> &processorDataExports() const {
    return ProcessorDataExports;
  }
  const std::map<std::string, uint64_t> &nlsDataExports() const {
    return NlsDataExports;
  }
  const std::map<std::string, uint64_t> &halDispatchExports() const {
    return HalDispatchExports;
  }

private:
  std::vector<ImageMapping> Code;
  std::vector<ImageMapping> OpaqueData;
  KernelReferenceExports ExportData;
  std::map<std::string, uint64_t> Exports;
  std::map<std::string, uint64_t> ThreadInitializationSymbols;
  std::map<std::string, uint64_t> ObjectTypeExports;
  std::map<std::string, uint64_t> ProcessDataExports;
  std::map<std::string, uint64_t> ProcessorDataExports;
  std::map<std::string, uint64_t> NlsDataExports;
  std::map<std::string, uint64_t> HalDispatchExports;
  uint64_t Base = 0, Size = 0;
  std::string Digest;
  std::string PDBDigest;
  std::optional<DebuggerFacts> Debugger;
};
} // namespace neverd::emulation
#endif
