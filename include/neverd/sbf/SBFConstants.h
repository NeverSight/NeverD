//===- SBFConstants.h - Solana SBF protocol constants -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SBFCONSTANTS_H
#define NEVERD_SBF_SBFCONSTANTS_H

#include "llvm/ADT/StringRef.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace neverd::sbf {

inline constexpr uint16_t kELFMachineBPF = 247;
inline constexpr uint16_t kELFMachineSBPF = 263;

inline constexpr size_t kInstructionSize = 8;
inline constexpr size_t kOpcodeOffset = 0;
inline constexpr size_t kRegisterByteOffset = 1;
inline constexpr size_t kBranchOffsetOffset = 2;
inline constexpr size_t kImmediateOffset = 4;
#define SBF_PROTOCOL_LIMIT(NAME, VALUE, SOURCE)                                \
  inline constexpr size_t k##NAME = VALUE;
#include "neverd/sbf/SBFProtocolLimits.def"
inline constexpr size_t kMaxInstructions =
    kMaxProgramAccountDataSize / kInstructionSize;
static_assert(kMaxProgramAccountDataSize % kInstructionSize == 0,
              "the SBF account-data limit must contain whole instructions");
static_assert(kMaxInstructions > kLegacyProgramInstructionCount,
              "the deployable-program bound must exceed the legacy cap");
inline constexpr unsigned kLDDWSlotCount = 2;
inline constexpr unsigned kBitsPerByte = std::numeric_limits<uint8_t>::digits;
inline constexpr unsigned kHalfWordBitWidth =
    std::numeric_limits<uint16_t>::digits;
inline constexpr unsigned kWordBitWidth = std::numeric_limits<uint32_t>::digits;
inline constexpr unsigned kDoubleWordBitWidth =
    std::numeric_limits<uint64_t>::digits;

inline constexpr unsigned kRegisterEncodingBits = 4;
inline constexpr uint8_t kRegisterEncodingMask =
    (uint8_t{1} << kRegisterEncodingBits) - 1;

inline constexpr unsigned kRegisterCount = 11;
inline constexpr unsigned kReturnRegister = 0;
inline constexpr std::array kArgumentRegisters = {
#define SBF_ARGUMENT_REGISTER(ID, REGISTER) unsigned{REGISTER},
#include "neverd/sbf/SBFArgumentRegisters.def"
};
inline constexpr unsigned kFirstArgumentRegister = kArgumentRegisters.front();
/// The register the entrypoint receives the address of the instruction data
/// in, on a runtime that has activated it. It is the second argument register
/// because a program's entrypoint is called like any other function.
inline constexpr unsigned kInstructionDataRegister = kFirstArgumentRegister + 1;
inline constexpr unsigned kArgumentRegisterCount = kArgumentRegisters.size();
inline constexpr unsigned kFramePointerRegister = kRegisterCount - 1;
inline constexpr unsigned kFirstCalleeSavedRegister =
    kFirstArgumentRegister + kArgumentRegisterCount;
inline constexpr unsigned kCalleeSavedRegisterCount =
    kFramePointerRegister - kFirstCalleeSavedRegister;
inline constexpr unsigned kProgramCounterRegister = kRegisterCount;
inline constexpr unsigned kVMRegisterCount = kProgramCounterRegister + 1;

constexpr bool argumentRegistersAreContiguous() {
  for (size_t Index = 0; Index < kArgumentRegisters.size(); ++Index)
    if (kArgumentRegisters[Index] != kFirstArgumentRegister + Index)
      return false;
  return true;
}

static_assert(argumentRegistersAreContiguous(),
              "SBF argument registers must form a contiguous ABI range");
static_assert(kFirstArgumentRegister == kReturnRegister + 1,
              "SBF arguments must immediately follow the return register");
static_assert(kFirstCalleeSavedRegister + kCalleeSavedRegisterCount ==
                  kFramePointerRegister,
              "SBF callee-saved registers must end at the frame pointer");

inline constexpr size_t kDefaultStackFrameSize = 4'096;
inline constexpr size_t kDefaultMaxCallDepth = 64;
inline constexpr size_t kStackFrameGapMultiplier = 2;
inline constexpr size_t kDynamicStackFrameAlignment = 64;
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

inline constexpr unsigned kRuntimeEnvironmentParameter = 0;
inline constexpr unsigned kRuntimeLoadAddressParameter =
    kRuntimeEnvironmentParameter + 1;
inline constexpr unsigned kRuntimeLoadWidthParameter =
    kRuntimeLoadAddressParameter + 1;
inline constexpr unsigned kRuntimeLoadOutputParameter =
    kRuntimeLoadWidthParameter + 1;
inline constexpr unsigned kRuntimeLoadArgumentCount =
    kRuntimeLoadOutputParameter + 1;
inline constexpr unsigned kRuntimeStoreAddressParameter =
    kRuntimeEnvironmentParameter + 1;
inline constexpr unsigned kRuntimeStoreWidthParameter =
    kRuntimeStoreAddressParameter + 1;
inline constexpr unsigned kRuntimeStoreValueParameter =
    kRuntimeStoreWidthParameter + 1;
inline constexpr unsigned kRuntimeStoreArgumentCount =
    kRuntimeStoreValueParameter + 1;
inline constexpr unsigned kRuntimeSyscallHashParameter =
    kRuntimeEnvironmentParameter + 1;
inline constexpr unsigned kRuntimeSyscallFirstArgumentParameter =
    kRuntimeSyscallHashParameter + 1;
inline constexpr unsigned kRuntimeSyscallOutputParameter =
    kRuntimeSyscallFirstArgumentParameter + kArgumentRegisterCount;
inline constexpr unsigned kRuntimeSyscallArgumentCount =
    kRuntimeSyscallOutputParameter + 1;
inline constexpr unsigned kRuntimeFaultCodeParameter =
    kRuntimeEnvironmentParameter + 1;
inline constexpr unsigned kRuntimeFaultAddressParameter =
    kRuntimeFaultCodeParameter + 1;
inline constexpr unsigned kRuntimeFaultArgumentCount =
    kRuntimeFaultAddressParameter + 1;

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
