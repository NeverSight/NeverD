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

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace neverd::sbf::test {

inline llvm::StringRef rustError(FaultCode Fault) {
  switch (Fault) {
  case FaultCode::InvalidInstruction:
  case FaultCode::InvalidRegister:
  case FaultCode::InvalidBranch:
    return "SbfError::InvalidInstruction";
  case FaultCode::DivideByZero:
    return "SbfError::DivideByZero";
  case FaultCode::DivideOverflow:
    return "SbfError::DivideOverflow";
  case FaultCode::MemoryAccess:
    return "SbfError::MemoryAccess";
  case FaultCode::CallDepth:
    return "SbfError::CallDepth";
  case FaultCode::UnknownSyscall:
    return "SbfError::UnknownSyscall";
  case FaultCode::UnknownIndirectCall:
    return "SbfError::UnknownFunction";
  case FaultCode::ExecutionOverrun:
    return "SbfError::ExecutionOverrun";
  case FaultCode::None:
    break;
  }
  return "SbfError::InvalidInstruction";
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

inline std::string makeRustHarness(const ExecutionEnvironment &Environment,
                                   const ExecutionResult &Expected,
                                   const std::vector<MemoryRegion> &Memory) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  OS << "\nstruct Region { address: u64, bytes: Vec<u8>, writable: bool }\n"
        "struct Env { regions: Vec<Region>, syscalls: Vec<(u32, [u64; "
     << kArgumentRegisterCount << "], u64)> }\n"
     << "const HASH_OFFSET: u64 = " << hexWord(kHashOffset) << ";\n"
     << "const HASH_PRIME: u64 = " << hexWord(kHashPrime) << ";\n\n"
     << R"(impl SbfEnvironment for Env {
    fn load(&mut self, address: u64, width: u8) -> Result<u64, SbfError> {
        if width == 0 || width % 8 != 0 { return Err(SbfError::MemoryAccess); }
        let size = usize::from(width / 8);
        if size > core::mem::size_of::<u64>() {
            return Err(SbfError::MemoryAccess);
        }
        for region in &self.regions {
            if address < region.address { continue; }
            let delta = address - region.address;
            let Ok(offset) = usize::try_from(delta) else { continue; };
            if offset > region.bytes.len() || size > region.bytes.len() - offset {
                continue;
            }
            let mut value = 0u64;
            for byte in 0..size {
                value |= u64::from(region.bytes[offset + byte]) << (byte * 8);
            }
            return Ok(value);
        }
        Err(SbfError::MemoryAccess)
    }

    fn store(&mut self, address: u64, width: u8, value: u64)
        -> Result<(), SbfError> {
        if width == 0 || width % 8 != 0 { return Err(SbfError::MemoryAccess); }
        let size = usize::from(width / 8);
        if size > core::mem::size_of::<u64>() {
            return Err(SbfError::MemoryAccess);
        }
        for region in &mut self.regions {
            if address < region.address { continue; }
            let delta = address - region.address;
            let Ok(offset) = usize::try_from(delta) else { continue; };
            if offset > region.bytes.len() || size > region.bytes.len() - offset {
                continue;
            }
            if !region.writable { return Err(SbfError::MemoryAccess); }
            for byte in 0..size {
                region.bytes[offset + byte] = (value >> (byte * 8)) as u8;
            }
            return Ok(());
        }
        Err(SbfError::MemoryAccess)
    }

)";
  OS << "    fn syscall(&mut self, hash: u32, args: [u64; "
     << kArgumentRegisterCount << "])\n";
  OS << R"(        -> Result<u64, SbfError> {
        let result = args[0].wrapping_add(1);
        self.syscalls.push((hash, args, result));
        Ok(result)
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
    OS << ", writable: " << (Region.Writable ? "true" : "false") << " },\n";
  }
  OS << "    ], syscalls: Vec::new() };\n"
     << "    let result = " << kEntryFunctionName << "(&mut env, "
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
