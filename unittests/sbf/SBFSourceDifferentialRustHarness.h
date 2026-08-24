//===- SBFSourceDifferentialRustHarness.h - Generated Rust harness -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The Rust main() that drives an emitted program and checks it against what
/// the interpreter recorded.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_SBF_SBFSOURCEDIFFERENTIALRUSTHARNESS_H
#define NEVERD_UNITTESTS_SBF_SBFSOURCEDIFFERENTIALRUSTHARNESS_H

#include "SBFSourceDifferentialDetail.h"

#include "neverd/sbf/emit/SBFSourceStatus.h"

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace neverd::sbf::test {

inline llvm::StringRef rustError(FaultCode Fault) {
  return rustSourceErrorName(Fault);
}

inline void emitRustBytes(llvm::raw_ostream &OS, const MemoryRegion &Region) {
  if (isZeroFilled(Region)) {
    OS << "vec![0u8; " << Region.Bytes.size() << "]";
    return;
  }
  OS << "vec![";
  for (size_t I = 0; I < Region.Bytes.size(); ++I) {
    if (I)
      OS << ",";
    OS << unsigned(Region.Bytes[I]) << "u8";
  }
  OS << "]";
}

inline std::string makeRustHarness(
    const ExecutionEnvironment &Environment, const ExecutionResult &Expected,
    RuntimeFeature ProgramRuntimeFeatures,
    const std::vector<MemoryRegion> &Memory,
    const std::vector<DifferentialCase::HostFault> &HostFaults,
    std::optional<FaultCode> LoadFault, std::optional<FaultCode> StoreFault) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  const RuntimeFeature ResolvedRuntimeFeatures =
      Environment.RuntimeFeatures.value_or(ProgramRuntimeFeatures);
  OS << "\nstruct Region { address: u64, bytes: Vec<u8>, gap_size: usize, "
        "writable: bool }\n"
        "struct Env { regions: Vec<Region>, syscalls: Vec<(u32, [u64; "
     << kArgumentRegisterCount
     << "], u64)>, load_fault: Option<SbfErrorV2>, store_fault: "
        "Option<SbfErrorV2>, runtime_features: Option<SbfRuntimeFeatures>, "
        "expected_runtime_features: SbfRuntimeFeatures }\n"
     << "const HASH_OFFSET: u64 = " << hexWord(kHashOffset) << ";\n"
     << "const HASH_PRIME: u64 = " << hexWord(kHashPrime) << ";\n\n"
     << "const GAP_MULTIPLIER: u64 = " << kStackFrameGapMultiplier << "u64;\n\n"
     << R"(fn region_offset(region: &Region, address: u64, size: usize) -> Option<usize> {
    let delta = address.checked_sub(region.address)?;
    if region.gap_size == 0 {
        let offset = usize::try_from(delta).ok()?;
        return (offset <= region.bytes.len() &&
                size <= region.bytes.len() - offset).then_some(offset);
    }
    if region.bytes.len() % region.gap_size != 0 { return None; }
    let backing_size = u64::try_from(region.bytes.len()).ok()?;
    let gap_size = u64::try_from(region.gap_size).ok()?;
    let span = backing_size.checked_mul(GAP_MULTIPLIER)?;
    if delta >= span { return None; }
    let period = gap_size.checked_mul(GAP_MULTIPLIER)?;
    let period_offset = delta % period;
    let size64 = u64::try_from(size).ok()?;
    if period_offset >= gap_size || size64 > gap_size - period_offset {
        return None;
    }
    let backing_offset = (delta / period)
        .checked_mul(gap_size)?.checked_add(period_offset)?;
    let offset = usize::try_from(backing_offset).ok()?;
    (offset <= region.bytes.len() &&
     size <= region.bytes.len() - offset).then_some(offset)
}

impl SbfEnvironmentV2 for Env {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfErrorV2> {
        if let Some(error) = self.load_fault { return Err(error); }
        if width == 0 || width % 8 != 0 { return Err(SbfErrorV2::MemoryAccess); }
        let size = usize::from(width / 8);
        if size > core::mem::size_of::<u64>() {
            return Err(SbfErrorV2::MemoryAccess);
        }
        for region in &self.regions {
            let Some(offset) = region_offset(region, address, size) else { continue; };
            let mut value = 0u64;
            for byte in 0..size {
                value |= u64::from(region.bytes[offset + byte]) << (byte * 8);
            }
            return Ok(value);
        }
        Err(SbfErrorV2::MemoryAccess)
    }

    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfErrorV2> {
        if let Some(error) = self.store_fault { return Err(error); }
        if width == 0 || width % 8 != 0 { return Err(SbfErrorV2::MemoryAccess); }
        let size = usize::from(width / 8);
        if size > core::mem::size_of::<u64>() {
            return Err(SbfErrorV2::MemoryAccess);
        }
        for region in &mut self.regions {
            let Some(offset) = region_offset(region, address, size) else { continue; };
            if !region.writable { return Err(SbfErrorV2::MemoryAccess); }
            for byte in 0..size {
                region.bytes[offset + byte] = (value >> (byte * 8)) as u8;
            }
            return Ok(());
        }
        Err(SbfErrorV2::MemoryAccess)
}

)";
  OS << "    fn syscall(&mut self, hash: u32, args: [u64; "
     << kArgumentRegisterCount << "])\n";
  OS << "        -> Result<u64, SbfErrorV2> {\n";
  for (const DifferentialCase::HostFault &Fault : HostFaults)
    OS << "        if hash == 0x" << llvm::utohexstr(Fault.Hash)
       << "u32 { return Err(" << rustSourceErrorName(Fault.Fault) << "); }\n";
  OS << "        if !matches!(hash, ";
  if (Expected.Syscalls.empty()) {
    OS << "_ if false";
  } else {
    for (size_t I = 0; I < Expected.Syscalls.size(); ++I) {
      if (I)
        OS << " | ";
      OS << "0x" << llvm::utohexstr(Expected.Syscalls[I].Hash) << "u32";
    }
  }
  OS << ") { return Err(SbfErrorV2::UnknownSyscall); }\n";
  OS << R"(        let result = args[0].wrapping_add(1);
        self.syscalls.push((hash, args, result));
        Ok(result)
    }

    fn runtime_features(&self) -> Option<SbfRuntimeFeatures> {
        self.runtime_features
    }

    fn syscall_with_features(
        &mut self, invocation: SbfSyscallInvocation
    ) -> SbfSyscallOutcomeV2 {
        assert_eq!(invocation.runtime_features, self.expected_runtime_features);
        self.syscall_outcome(invocation.hash, invocation.args)
    }
}

fn writable_hash(regions: &[Region]) -> u64 {
    let mut hash = HASH_OFFSET;
    for region in regions.iter().filter(|region| region.writable) {
        for byte in &region.bytes {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(HASH_PRIME);
        }
    }
    hash
}

fn main() {
    let mut env = Env { regions: vec![
)";
  for (const MemoryRegion &Region : Memory) {
    OS << "        Region { address: " << hexWord(Region.Address)
       << ", bytes: ";
    emitRustBytes(OS, Region);
    OS << ", gap_size: " << Region.VMGapSize
       << "usize, writable: " << (Region.Writable ? "true" : "false")
       << " },\n";
  }
  OS << "    ], syscalls: Vec::new(), load_fault: ";
  if (LoadFault)
    OS << "Some(" << rustSourceErrorName(*LoadFault) << ")";
  else
    OS << "None";
  OS << ", store_fault: ";
  if (StoreFault)
    OS << "Some(" << rustSourceErrorName(*StoreFault) << ")";
  else
    OS << "None";
  OS << ", runtime_features: ";
  if (Environment.RuntimeFeatures)
    OS << "Some(SbfRuntimeFeatures::from_bits("
       << rustRuntimeFeatureMaskWord(
              runtimeFeatureMask(*Environment.RuntimeFeatures))
       << "))";
  else
    OS << "None";
  OS << ", expected_runtime_features: SbfRuntimeFeatures::from_bits("
     << rustRuntimeFeatureMaskWord(runtimeFeatureMask(ResolvedRuntimeFeatures))
     << ") };\n"
     << "    let result = " << kEntryFunctionName << "_v2(&mut env, "
     << hexWord(Environment.Input) << ", "
     << hexWord(Environment.InstructionData) << ");\n";
  if (Expected.Status == ExecutionStatus::Returned)
    OS << "    assert_eq!(result, Ok(" << hexWord(Expected.ReturnValue)
       << "));\n";
  else
    OS << "    assert_eq!(result, Err(" << rustError(Expected.Fault) << "));\n";
  OS << "    assert_eq!(writable_hash(&env.regions), "
     << hexWord(hashWritableMemory(Expected.Memory)) << ");\n"
     << "    assert_eq!(env.syscalls.len(), " << Expected.Syscalls.size()
     << "usize);\n";
  for (size_t I = 0; I < Expected.Syscalls.size(); ++I) {
    const SyscallTraceEntry &Trace = Expected.Syscalls[I];
    OS << "    assert_eq!(env.syscalls[" << I << "].0, 0x"
       << llvm::utohexstr(Trace.Hash) << "u32);\n"
       << "    assert_eq!(env.syscalls[" << I << "].1, [";
    for (size_t Argument = 0; Argument < Trace.Arguments.size(); ++Argument) {
      if (Argument)
        OS << ",";
      OS << hexWord(Trace.Arguments[Argument]);
    }
    OS << "]);\n"
       << "    assert_eq!(env.syscalls[" << I << "].2, "
       << hexWord(Trace.Result) << ");\n";
  }
  OS << "}\n";
  return Buffer;
}

} // namespace neverd::sbf::test

#endif // NEVERD_UNITTESTS_SBF_SBFSOURCEDIFFERENTIALRUSTHARNESS_H
