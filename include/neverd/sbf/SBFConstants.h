//===- SBFConstants.h - Solana SBF protocol constants ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SBFCONSTANTS_H
#define NEVERD_SBF_SBFCONSTANTS_H

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>

namespace neverd::sbf {

inline constexpr uint16_t kELFMachineBPF = 247;
inline constexpr uint16_t kELFMachineSBPF = 263;

inline constexpr size_t kInstructionSize = 8;
inline constexpr size_t kOpcodeOffset = 0;
inline constexpr size_t kRegisterByteOffset = 1;
inline constexpr size_t kBranchOffsetOffset = 2;
inline constexpr size_t kImmediateOffset = 4;
inline constexpr size_t kMaxInstructions = 65'536;

inline constexpr unsigned kRegisterEncodingBits = 4;
inline constexpr uint8_t kRegisterEncodingMask =
    (uint8_t{1} << kRegisterEncodingBits) - 1;

inline constexpr unsigned kRegisterCount = 11;
inline constexpr unsigned kVMRegisterCount = 12;
inline constexpr unsigned kReturnRegister = 0;
inline constexpr unsigned kFirstArgumentRegister = 1;
inline constexpr unsigned kArgumentRegisterCount = 5;
inline constexpr unsigned kFirstCalleeSavedRegister = 6;
inline constexpr unsigned kCalleeSavedRegisterCount = 4;
inline constexpr unsigned kFramePointerRegister = 10;
inline constexpr unsigned kProgramCounterRegister = 11;

inline constexpr size_t kDefaultStackFrameSize = 4'096;
inline constexpr size_t kDefaultMaxCallDepth = 64;
inline constexpr size_t kStackFrameGapMultiplier = 2;
inline constexpr size_t kDynamicStackFrameAlignment = 64;
inline constexpr size_t kDefaultStackSize =
    kDefaultStackFrameSize * kDefaultMaxCallDepth;
inline constexpr size_t kDefaultMaxExecutionSteps = 1'000'000;

inline constexpr unsigned kVirtualAddressBits = 32;
inline constexpr uint64_t kMemoryRegionSize = uint64_t{1}
                                              << kVirtualAddressBits;
inline constexpr uint64_t kRodataStartV3 = 0;
inline constexpr uint64_t kBytecodeStart = kMemoryRegionSize;
inline constexpr uint64_t kStackStart = kMemoryRegionSize * 2;
inline constexpr uint64_t kHeapStart = kMemoryRegionSize * 3;
inline constexpr uint64_t kInputStart = kMemoryRegionSize * 4;

inline constexpr llvm::StringLiteral kELFMachineBPFName("EM_BPF");
inline constexpr llvm::StringLiteral kELFMachineSBPFName("EM_SBPF");
inline constexpr llvm::StringLiteral kUnknownELFMachineName("unknown");
inline constexpr llvm::StringLiteral kLegacyLayoutName("legacy-section");
inline constexpr llvm::StringLiteral kStrictLayoutName("strict-program-header");
inline constexpr llvm::StringLiteral kTextSectionName(".text");
inline constexpr llvm::StringLiteral kRodataSectionName(".rodata");
inline constexpr llvm::StringLiteral kDataSectionPrefix(".data");
inline constexpr llvm::StringLiteral kBSSSectionPrefix(".bss");
inline constexpr llvm::StringLiteral kDataRelSectionPrefix(".data.rel");
inline constexpr llvm::StringLiteral kDataRelROSectionName(".data.rel.ro");
inline constexpr llvm::StringLiteral kEhFrameSectionName(".eh_frame");
inline constexpr llvm::StringLiteral kEntrySymbolName("entrypoint");
inline constexpr llvm::StringLiteral kTextSegmentName("SBF_TEXT");
inline constexpr llvm::StringLiteral kRodataSegmentName("SBF_RODATA");
inline constexpr llvm::StringLiteral kUnknownSyscallName("unknown_syscall");
inline constexpr llvm::StringLiteral kUnknownFunctionName("unknown_function");
inline constexpr llvm::StringLiteral kModuleName("neverd_sbf_output");
inline constexpr llvm::StringLiteral kEntryFunctionName("neverd_sbf_program");
inline constexpr llvm::StringLiteral kRuntimeLoadName("neverd_sbf_load");
inline constexpr llvm::StringLiteral kRuntimeStoreName("neverd_sbf_store");
inline constexpr llvm::StringLiteral kRuntimeSyscallName("neverd_sbf_syscall");
inline constexpr llvm::StringLiteral kRuntimeCallXName("neverd_sbf_callx");
inline constexpr llvm::StringLiteral kRuntimeFaultName("neverd_sbf_fault");

enum class FaultCode : uint32_t {
  None = 0,
  InvalidInstruction,
  InvalidRegister,
  InvalidBranch,
  DivideByZero,
  DivideOverflow,
  MemoryAccess,
  CallDepth,
  UnknownSyscall,
  UnknownIndirectCall,
  ExecutionOverrun,
};

} // namespace neverd::sbf

#endif // NEVERD_SBF_SBFCONSTANTS_H
