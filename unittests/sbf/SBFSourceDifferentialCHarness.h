//===- SBFSourceDifferentialCHarness.h - Generated C test harness --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The C main() that drives an emitted program and checks it against what the
/// interpreter recorded.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_SBF_SBFSOURCEDIFFERENTIALCHARNESS_H
#define NEVERD_UNITTESTS_SBF_SBFSOURCEDIFFERENTIALCHARNESS_H

#include "SBFSourceDifferentialDetail.h"

#include "neverd/sbf/emit/SBFSourceStatus.h"

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace neverd::sbf::test {

inline llvm::StringRef cStatus(const ExecutionResult &Result) {
  if (Result.Status == ExecutionStatus::Returned)
    return cSourceStatusName(SourceStatus::Ok);
  if (Result.Fault == FaultCode::None)
    llvm_unreachable("faulted SBF source oracle has no fault code");
  return cSourceStatusName(Result.Fault);
}

inline void emitCRegions(llvm::raw_ostream &OS,
                         const std::vector<MemoryRegion> &Memory) {
  for (size_t I = 0; I < Memory.size(); ++I) {
    OS << "static uint8_t region_" << I << "[" << Memory[I].Bytes.size()
       << "] = {";
    if (!isZeroFilled(Memory[I]))
      for (size_t Byte = 0; Byte < Memory[I].Bytes.size(); ++Byte) {
        if (Byte)
          OS << ",";
        OS << unsigned(Memory[I].Bytes[Byte]);
      }
    else
      OS << "0";
    OS << "};\n";
  }
  OS << "typedef struct runtime_region { uint64_t address; size_t size; "
        "size_t gap_size; int writable; uint8_t *bytes; } runtime_region;\n"
        "static runtime_region runtime_regions[] = {\n";
  for (size_t I = 0; I < Memory.size(); ++I)
    OS << "  { UINT64_C(0x" << llvm::utohexstr(Memory[I].Address)
       << "), sizeof(region_" << I << "), " << Memory[I].VMGapSize << "u, "
       << (Memory[I].Writable ? 1 : 0) << ", region_" << I << " },\n";
  OS << "};\n";
}

inline std::string makeCHarness(
    const ExecutionEnvironment &Environment, const ExecutionResult &Expected,
    RuntimeFeature ProgramRuntimeFeatures,
    const std::vector<MemoryRegion> &Memory,
    const std::vector<DifferentialCase::HostFault> &HostFaults,
    std::optional<FaultCode> LoadFault, std::optional<FaultCode> StoreFault) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  const RuntimeFeature ResolvedRuntimeFeatures =
      Environment.RuntimeFeatures.value_or(ProgramRuntimeFeatures);
  emitCRegions(OS, Memory);
  OS << "enum { RUNTIME_TRACE_CAPACITY = " << kRuntimeTraceCapacity
     << ", RUNTIME_ARGUMENT_COUNT = " << kArgumentRegisterCount
     << ", RUNTIME_BITS_PER_BYTE = " << kBitsPerByte
     << ", RUNTIME_GAP_MULTIPLIER = " << kStackFrameGapMultiplier << " };\n"
     << "#define RUNTIME_HASH_OFFSET UINT64_C(0x"
     << llvm::utohexstr(kHashOffset) << ")\n"
     << "#define RUNTIME_HASH_PRIME UINT64_C(0x" << llvm::utohexstr(kHashPrime)
     << ")\n"
     << "#define RUNTIME_EXPECTED_FEATURES "
        "((neverd_sbf_runtime_feature_mask)"
     << cRuntimeFeatureMaskWord(runtimeFeatureMask(ResolvedRuntimeFeatures))
     << ")\n";
  OS << R"(
typedef struct runtime_context {
  uint32_t hashes[RUNTIME_TRACE_CAPACITY];
  uint64_t arguments[RUNTIME_TRACE_CAPACITY][RUNTIME_ARGUMENT_COUNT];
  uint64_t results[RUNTIME_TRACE_CAPACITY];
  size_t syscall_count;
  neverd_sbf_status_v2 load_fault;
  neverd_sbf_status_v2 store_fault;
} runtime_context;

static int runtime_range(runtime_region *region, uint64_t address, size_t size,
                         size_t *offset) {
  uint64_t backing_offset, delta, period, period_offset, span;
  if (address < region->address) return 0;
  delta = address - region->address;
  if (region->gap_size == 0) {
    if (delta > region->size || size > region->size - (size_t)delta) return 0;
    *offset = (size_t)delta;
    return 1;
  }
  if (region->size % region->gap_size != 0 ||
      region->size > UINT64_MAX / RUNTIME_GAP_MULTIPLIER ||
      region->gap_size > UINT64_MAX / RUNTIME_GAP_MULTIPLIER)
    return 0;
  span = (uint64_t)region->size * RUNTIME_GAP_MULTIPLIER;
  if (delta >= span) return 0;
  period = (uint64_t)region->gap_size * RUNTIME_GAP_MULTIPLIER;
  period_offset = delta % period;
  if (period_offset >= region->gap_size ||
      size > region->gap_size - (size_t)period_offset)
    return 0;
  backing_offset = delta / period * region->gap_size + period_offset;
  if (backing_offset > region->size ||
      size > region->size - (size_t)backing_offset)
    return 0;
  *offset = (size_t)backing_offset;
  return 1;
}

static int runtime_load(void *opaque, uint64_t address, uint32_t width,
                        uint64_t *value) {
  size_t region_index, offset, byte, size;
  runtime_context *context = (runtime_context *)opaque;
  if (context && context->load_fault != NEVERD_SBF_OK)
    return (int)context->load_fault;
  if (!value || width == 0 || width % RUNTIME_BITS_PER_BYTE != 0)
    return NEVERD_SBF_MEMORY_ACCESS;
  size = width / RUNTIME_BITS_PER_BYTE;
  if (size > sizeof(*value)) return NEVERD_SBF_MEMORY_ACCESS;
  for (region_index = 0;
       region_index < sizeof(runtime_regions) / sizeof(runtime_regions[0]);
       ++region_index) {
    if (!runtime_range(&runtime_regions[region_index], address, size, &offset))
      continue;
    *value = 0;
    for (byte = 0; byte < size; ++byte)
      *value |= (uint64_t)runtime_regions[region_index].bytes[offset + byte]
                << (byte * RUNTIME_BITS_PER_BYTE);
    return NEVERD_SBF_OK;
  }
  return NEVERD_SBF_MEMORY_ACCESS;
}

static int runtime_store(void *opaque, uint64_t address, uint32_t width,
                         uint64_t value) {
  size_t region_index, offset, byte, size;
  runtime_context *context = (runtime_context *)opaque;
  if (context && context->store_fault != NEVERD_SBF_OK)
    return (int)context->store_fault;
  if (width == 0 || width % RUNTIME_BITS_PER_BYTE != 0)
    return NEVERD_SBF_MEMORY_ACCESS;
  size = width / RUNTIME_BITS_PER_BYTE;
  if (size > sizeof(value)) return NEVERD_SBF_MEMORY_ACCESS;
  for (region_index = 0;
       region_index < sizeof(runtime_regions) / sizeof(runtime_regions[0]);
       ++region_index) {
    if (!runtime_range(&runtime_regions[region_index], address, size, &offset))
      continue;
    if (!runtime_regions[region_index].writable)
      return NEVERD_SBF_MEMORY_ACCESS;
    for (byte = 0; byte < size; ++byte)
      runtime_regions[region_index].bytes[offset + byte] =
          (uint8_t)(value >> (byte * RUNTIME_BITS_PER_BYTE));
    return NEVERD_SBF_OK;
  }
  return NEVERD_SBF_MEMORY_ACCESS;
}
)";
  OS << "\nstatic int runtime_syscall(void *opaque, uint32_t hash";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    OS << ", uint64_t a" << Index;
  OS << ", uint64_t *value) {\n"
        "  runtime_context *context = (runtime_context *)opaque;\n"
        "  uint64_t arguments[RUNTIME_ARGUMENT_COUNT] = {";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index) {
    if (Index)
      OS << ", ";
    OS << "a" << Index;
  }
  OS << R"(};
  size_t index, argument;
  if (!context || !value || context->syscall_count >= RUNTIME_TRACE_CAPACITY)
    return NEVERD_SBF_INVALID_INSTRUCTION;
)";
  for (const DifferentialCase::HostFault &Fault : HostFaults)
    OS << "  if (hash == UINT32_C(0x" << llvm::utohexstr(Fault.Hash)
       << ")) return " << cSourceStatusName(Fault.Fault) << ";\n";
  OS << "  if (";
  if (Expected.Syscalls.empty()) {
    OS << "1";
  } else {
    for (size_t I = 0; I < Expected.Syscalls.size(); ++I) {
      if (I)
        OS << " && ";
      OS << "hash != UINT32_C(0x" << llvm::utohexstr(Expected.Syscalls[I].Hash)
         << ")";
    }
  }
  OS << ") return NEVERD_SBF_UNKNOWN_SYSCALL;\n";
  OS << R"(  index = context->syscall_count++;
  context->hashes[index] = hash;
  for (argument = 0; argument < RUNTIME_ARGUMENT_COUNT; ++argument)
    context->arguments[index][argument] = arguments[argument];
  *value = arguments[0] + UINT64_C(1);
  context->results[index] = *value;
  return NEVERD_SBF_OK;
}

static int runtime_syscall_with_features(
    void *opaque, const neverd_sbf_syscall_invocation *invocation,
    uint64_t *value) {
  if (!invocation ||
      invocation->runtime_features.bits != RUNTIME_EXPECTED_FEATURES)
    return NEVERD_SBF_INVALID_INSTRUCTION;
  return runtime_syscall(opaque, invocation->hash,
)";
  for (unsigned Index = 0; Index < kArgumentRegisterCount; ++Index)
    OS << "      invocation->arguments[" << Index << "],\n";
  OS << R"(      value);
}

static uint64_t writable_hash(void) {
  uint64_t hash = RUNTIME_HASH_OFFSET;
  size_t region_index, byte;
  for (region_index = 0;
       region_index < sizeof(runtime_regions) / sizeof(runtime_regions[0]);
       ++region_index) {
    if (!runtime_regions[region_index].writable) continue;
    for (byte = 0; byte < runtime_regions[region_index].size; ++byte) {
      hash ^= runtime_regions[region_index].bytes[byte];
      hash *= RUNTIME_HASH_PRIME;
    }
  }
  return hash;
}

int main(void) {
  runtime_context context;
  neverd_sbf_environment_v2 environment;
  neverd_sbf_status_v2 status;
  uint64_t result = 0;
  size_t trace;
  memset(&context, 0, sizeof(context));
  memset(&environment, 0, sizeof(environment));
)";
  if (Environment.RuntimeFeatures)
    OS << "  neverd_sbf_runtime_features runtime_features = { "
          "RUNTIME_EXPECTED_FEATURES };\n";
  if (LoadFault)
    OS << "  context.load_fault = " << cSourceStatusName(*LoadFault) << ";\n";
  if (StoreFault)
    OS << "  context.store_fault = " << cSourceStatusName(*StoreFault) << ";\n";
  OS << R"(
  environment.base.context = &context;
  environment.base.load = runtime_load;
  environment.base.store = runtime_store;
  environment.base.syscall = runtime_syscall;
  environment.syscall_with_features = runtime_syscall_with_features;
)";
  if (Environment.RuntimeFeatures)
    OS << "  environment.runtime_features = &runtime_features;\n";
  OS << "  status = " << kEntryFunctionName << "_v2(&environment, UINT64_C(0x"
     << llvm::utohexstr(Environment.Input) << "), UINT64_C(0x"
     << llvm::utohexstr(Environment.InstructionData) << "), &result);\n"
     << "  if (status != " << cStatus(Expected) << ") return 10;\n";
  if (Expected.Status == ExecutionStatus::Returned)
    OS << "  if (result != UINT64_C(0x" << llvm::utohexstr(Expected.ReturnValue)
       << ")) return 11;\n";
  OS << "  if (writable_hash() != UINT64_C(0x"
     << llvm::utohexstr(hashWritableMemory(Expected.Memory))
     << ")) return 12;\n"
     << "  if (context.syscall_count != " << Expected.Syscalls.size()
     << "u) return 13;\n";
  for (size_t I = 0; I < Expected.Syscalls.size(); ++I) {
    const SyscallTraceEntry &Trace = Expected.Syscalls[I];
    OS << "  trace = " << I << "u;\n"
       << "  if (context.hashes[trace] != UINT32_C(0x"
       << llvm::utohexstr(Trace.Hash) << ")) return 14;\n";
    for (size_t Argument = 0; Argument < Trace.Arguments.size(); ++Argument)
      OS << "  if (context.arguments[trace][" << Argument << "] != UINT64_C(0x"
         << llvm::utohexstr(Trace.Arguments[Argument]) << ")) return 15;\n";
    OS << "  if (context.results[trace] != UINT64_C(0x"
       << llvm::utohexstr(Trace.Result) << ")) return 16;\n";
  }
  OS << "  (void)trace;\n  return 0;\n}\n";
  return Buffer;
}

} // namespace neverd::sbf::test

#endif // NEVERD_UNITTESTS_SBF_SBFSOURCEDIFFERENTIALCHARNESS_H
