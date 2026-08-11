//===- SBFSourceDifferentialTests.cpp - SBF source backend execution -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Support/BinaryLoading.h"
#include "neverd/sbf/Analyzer.h"
#include "neverd/sbf/CEmitter.h"
#include "neverd/sbf/Interpreter.h"
#include "neverd/sbf/RustEmitter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

constexpr unsigned kBitsPerByte = 8;
constexpr unsigned kRuntimeTraceCapacity = 16;
constexpr uint64_t kHashOffset = UINT64_C(0xcbf29ce484222325);
constexpr uint64_t kHashPrime = UINT64_C(0x100000001b3);

using EncodedInstruction = std::array<uint8_t, kInstructionSize>;

enum class SourceBackend : uint8_t { C, Rust };

llvm::StringRef compilerName(SourceBackend Backend) {
  return Backend == SourceBackend::C ? "clang" : "rustc";
}

struct DifferentialCase {
  std::string Name;
  SBFProgram Program;
  ExecutionEnvironment Environment;
};

class TemporaryFile {
public:
  explicit TemporaryFile(llvm::StringRef Extension) {
    Error = llvm::sys::fs::createTemporaryFile("neverd-sbf-source-diff",
                                               Extension, Path);
  }

  ~TemporaryFile() {
    if (!Path.empty())
      llvm::sys::fs::remove(Path);
  }

  llvm::StringRef str() const { return Path; }
  std::error_code error() const { return Error; }

private:
  llvm::SmallString<128> Path;
  std::error_code Error;
};

EncodedInstruction encode(Opcode ID, uint8_t Dst = 0, uint8_t Src = 0,
                          int16_t Offset = 0, int32_t Immediate = 0) {
  EncodedInstruction Bytes{};
  const OpcodeInfo *Info = getOpcodeInfo(ID);
  EXPECT_NE(Info, nullptr);
  if (!Info)
    return Bytes;
  Bytes[kOpcodeOffset] = Info->Encoding;
  Bytes[kRegisterByteOffset] =
      static_cast<uint8_t>((Src << kRegisterEncodingBits) | Dst);
  llvm::support::endian::write16le(Bytes.data() + kBranchOffsetOffset,
                                   static_cast<uint16_t>(Offset));
  llvm::support::endian::write32le(Bytes.data() + kImmediateOffset,
                                   static_cast<uint32_t>(Immediate));
  return Bytes;
}

std::array<EncodedInstruction, kLDDWSlotCount> encodeLDDW(uint8_t Dst,
                                                          uint64_t Immediate) {
  std::array<EncodedInstruction, 2> Result{
      encode(Opcode::LDDW, Dst, 0, 0, static_cast<int32_t>(Immediate)), {}};
  llvm::support::endian::write32le(
      Result[1].data() + kImmediateOffset,
      static_cast<uint32_t>(Immediate >>
                            std::numeric_limits<uint32_t>::digits));
  return Result;
}

llvm::Expected<SBFProgram>
analyzeProgram(Version TheVersion,
               std::initializer_list<EncodedInstruction> Instructions) {
  BinaryImage Image;
  Image.Arch = Arch::SBF;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Entry = kBytecodeStart;
  for (const EncodedInstruction &Instruction : Instructions)
    Image.Raw.insert(Image.Raw.end(), Instruction.begin(), Instruction.end());

  Section Text;
  Text.Name = kTextSectionName.str();
  Text.VA = kBytecodeStart;
  Text.Size = Image.Raw.size();
  Text.FileSz = Image.Raw.size();
  Text.Flags = SegmentFlags::Executable;
  Text.Alignment = kInstructionSize;
  Text.Data = Image.Raw;
  Image.Sections.push_back(std::move(Text));

  Metadata Meta;
  Meta.Machine = kELFMachineBPF;
  Meta.ELFFlags = static_cast<uint32_t>(TheVersion);
  Meta.Version = TheVersion;
  Meta.StrictLayout = versionHasFeature(TheVersion, VersionFeature::StrictELF);
  Meta.TextFile = {0, Image.Raw.size()};
  Meta.TextVM = {kBytecodeStart, Image.Raw.size()};
  Image.SBF = Meta;
  return analyze(Image);
}

std::vector<MemoryRegion>
initialMemory(const SBFProgram &Program,
              const ExecutionEnvironment &Environment) {
  std::vector<MemoryRegion> Memory = Environment.Memory;
  for (const ProgramRegion &Region : Program.ExecutableImage.regions())
    if (Region.DataVisible && !Region.Bytes.empty())
      Memory.push_back({Region.Address, Region.Bytes, false, Region.Name});

  if (usesStackFrameGaps(Program.Low.TheVersion, Program.Config)) {
    for (size_t Frame = 0; Frame < Program.Config.MaxCallDepth; ++Frame) {
      MemoryRegion Stack;
      Stack.Address = kStackStart + Frame * Program.Config.StackFrameSize *
                                        kStackFrameGapMultiplier;
      Stack.Bytes.resize(Program.Config.StackFrameSize);
      Stack.Writable = true;
      Stack.Name = "stack." + std::to_string(Frame);
      Memory.push_back(std::move(Stack));
    }
  } else {
    Memory.push_back({kStackStart,
                      std::vector<uint8_t>(stackSize(Program.Config)), true,
                      "stack"});
  }
  return Memory;
}

uint64_t hashWritableMemory(const std::vector<MemoryRegion> &Memory) {
  uint64_t Hash = kHashOffset;
  for (const MemoryRegion &Region : Memory)
    if (Region.Writable)
      for (uint8_t Byte : Region.Bytes) {
        Hash ^= Byte;
        Hash *= kHashPrime;
      }
  return Hash;
}

llvm::StringRef cStatus(const ExecutionResult &Result) {
  if (Result.Status == ExecutionStatus::Returned)
    return "NEVERD_SBF_OK";
  switch (Result.Fault) {
  case FaultCode::InvalidInstruction:
  case FaultCode::InvalidRegister:
  case FaultCode::InvalidBranch:
    return "NEVERD_SBF_INVALID_INSTRUCTION";
  case FaultCode::DivideByZero:
    return "NEVERD_SBF_DIVIDE_BY_ZERO";
  case FaultCode::DivideOverflow:
    return "NEVERD_SBF_DIVIDE_OVERFLOW";
  case FaultCode::MemoryAccess:
    return "NEVERD_SBF_MEMORY_ACCESS";
  case FaultCode::CallDepth:
    return "NEVERD_SBF_CALL_DEPTH";
  case FaultCode::UnknownSyscall:
    return "NEVERD_SBF_UNKNOWN_SYSCALL";
  case FaultCode::UnknownIndirectCall:
    return "NEVERD_SBF_UNKNOWN_FUNCTION";
  case FaultCode::ExecutionOverrun:
    return "NEVERD_SBF_EXECUTION_OVERRUN";
  case FaultCode::None:
    break;
  }
  return "NEVERD_SBF_INVALID_INSTRUCTION";
}

llvm::StringRef rustError(FaultCode Fault) {
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

std::string hexWord(uint64_t Value) {
  return "0x" + llvm::utohexstr(Value) + "u64";
}

bool isZeroFilled(const MemoryRegion &Region) {
  return std::all_of(Region.Bytes.begin(), Region.Bytes.end(),
                     [](uint8_t Byte) { return Byte == 0; });
}

void emitCRegions(llvm::raw_ostream &OS,
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
  OS << "typedef struct runtime_region { uint64_t address; size_t size; int "
        "writable; uint8_t *bytes; } runtime_region;\n"
        "static runtime_region runtime_regions[] = {\n";
  for (size_t I = 0; I < Memory.size(); ++I)
    OS << "  { UINT64_C(0x" << llvm::utohexstr(Memory[I].Address)
       << "), sizeof(region_" << I << "), " << (Memory[I].Writable ? 1 : 0)
       << ", region_" << I << " },\n";
  OS << "};\n";
}

std::string makeCHarness(const ExecutionEnvironment &Environment,
                         const ExecutionResult &Expected,
                         const std::vector<MemoryRegion> &Memory) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  emitCRegions(OS, Memory);
  OS << "enum { RUNTIME_TRACE_CAPACITY = " << kRuntimeTraceCapacity
     << ", RUNTIME_ARGUMENT_COUNT = " << kArgumentRegisterCount
     << ", RUNTIME_BITS_PER_BYTE = " << kBitsPerByte << " };\n"
     << "#define RUNTIME_HASH_OFFSET UINT64_C(0x"
     << llvm::utohexstr(kHashOffset) << ")\n"
     << "#define RUNTIME_HASH_PRIME UINT64_C(0x" << llvm::utohexstr(kHashPrime)
     << ")\n";
  OS << R"(
typedef struct runtime_context {
  uint32_t hashes[RUNTIME_TRACE_CAPACITY];
  uint64_t arguments[RUNTIME_TRACE_CAPACITY][RUNTIME_ARGUMENT_COUNT];
  uint64_t results[RUNTIME_TRACE_CAPACITY];
  size_t syscall_count;
} runtime_context;

static int runtime_range(runtime_region *region, uint64_t address, size_t size,
                         size_t *offset) {
  uint64_t delta;
  if (address < region->address) return 0;
  delta = address - region->address;
  if (delta > region->size || size > region->size - (size_t)delta) return 0;
  *offset = (size_t)delta;
  return 1;
}

static int runtime_load(void *opaque, uint64_t address, uint32_t width,
                        uint64_t *value) {
  size_t region_index, offset, byte, size;
  (void)opaque;
  if (!value || width == 0 || width % RUNTIME_BITS_PER_BYTE != 0) return 1;
  size = width / RUNTIME_BITS_PER_BYTE;
  if (size > sizeof(*value)) return 1;
  for (region_index = 0;
       region_index < sizeof(runtime_regions) / sizeof(runtime_regions[0]);
       ++region_index) {
    if (!runtime_range(&runtime_regions[region_index], address, size, &offset))
      continue;
    *value = 0;
    for (byte = 0; byte < size; ++byte)
      *value |= (uint64_t)runtime_regions[region_index].bytes[offset + byte]
                << (byte * RUNTIME_BITS_PER_BYTE);
    return 0;
  }
  return 1;
}

static int runtime_store(void *opaque, uint64_t address, uint32_t width,
                         uint64_t value) {
  size_t region_index, offset, byte, size;
  (void)opaque;
  if (width == 0 || width % RUNTIME_BITS_PER_BYTE != 0) return 1;
  size = width / RUNTIME_BITS_PER_BYTE;
  if (size > sizeof(value)) return 1;
  for (region_index = 0;
       region_index < sizeof(runtime_regions) / sizeof(runtime_regions[0]);
       ++region_index) {
    if (!runtime_range(&runtime_regions[region_index], address, size, &offset))
      continue;
    if (!runtime_regions[region_index].writable) return 1;
    for (byte = 0; byte < size; ++byte)
      runtime_regions[region_index].bytes[offset + byte] =
          (uint8_t)(value >> (byte * RUNTIME_BITS_PER_BYTE));
    return 0;
  }
  return 1;
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
    return 1;
  index = context->syscall_count++;
  context->hashes[index] = hash;
  for (argument = 0; argument < RUNTIME_ARGUMENT_COUNT; ++argument)
    context->arguments[index][argument] = arguments[argument];
  *value = arguments[0] + UINT64_C(1);
  context->results[index] = *value;
  return 0;
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
  neverd_sbf_environment environment;
  neverd_sbf_status status;
  uint64_t result = 0;
  size_t trace;
  memset(&context, 0, sizeof(context));
  environment.context = &context;
  environment.load = runtime_load;
  environment.store = runtime_store;
  environment.syscall = runtime_syscall;
)";
  OS << "  status = " << kEntryFunctionName << "(&environment, UINT64_C(0x"
     << llvm::utohexstr(Environment.Input) << "), &result);\n"
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

void emitRustBytes(llvm::raw_ostream &OS, const MemoryRegion &Region) {
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

std::string makeRustHarness(const ExecutionEnvironment &Environment,
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
     << hexWord(Environment.Input) << ");\n";
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

void compileAndRun(SourceBackend Backend, llvm::StringRef Source) {
  const llvm::StringRef CompilerName = compilerName(Backend);
  auto Compiler = llvm::sys::findProgramByName(CompilerName);
  ASSERT_TRUE(static_cast<bool>(Compiler))
      << CompilerName.str() << " disappeared after capability detection";
  if (!Compiler)
    return;

  TemporaryFile SourceFile(Backend == SourceBackend::C ? "c" : "rs");
  TemporaryFile Executable("out");
  ASSERT_FALSE(SourceFile.error()) << SourceFile.error().message();
  ASSERT_FALSE(Executable.error()) << Executable.error().message();
  {
    std::ofstream Output(SourceFile.str().str(), std::ios::binary);
    ASSERT_TRUE(Output);
    Output << Source.str();
  }

  llvm::SmallVector<llvm::StringRef, 16> Arguments;
  Arguments.push_back(*Compiler);
  if (Backend == SourceBackend::C) {
    Arguments.append({"-std=c11", "-Wall", "-Wextra", "-Werror"});
  } else {
    Arguments.append({"--edition=2021",
                      "--crate-name=neverd_sbf_source_differential", "-D",
                      "warnings"});
  }
  Arguments.append({SourceFile.str(), "-o", Executable.str()});
  std::string Error;
  ASSERT_EQ(llvm::sys::ExecuteAndWait(*Compiler, Arguments, std::nullopt, {}, 0,
                                      0, &Error),
            0)
      << Error;
  llvm::SmallVector<llvm::StringRef, 1> RunArguments{Executable.str()};
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable.str(), RunArguments), 0);
}

void runDifferential(SourceBackend Backend, const DifferentialCase &Case) {
  SCOPED_TRACE(Case.Name);
  ExecutionEnvironment OracleEnvironment = Case.Environment;
  OracleEnvironment.Syscall = [](uint32_t, const SyscallArguments &Arguments)
      -> std::optional<uint64_t> { return Arguments[0] + 1; };
  std::vector<MemoryRegion> Memory =
      initialMemory(Case.Program, OracleEnvironment);
  auto Expected = executeRaw(Case.Program, std::move(OracleEnvironment));
  ASSERT_TRUE(static_cast<bool>(Expected))
      << llvm::toString(Expected.takeError());
  ASSERT_EQ(Expected->Memory.size(), Memory.size());

  if (Backend == SourceBackend::C) {
    CEmitterOptions Options;
    Options.PreferStructuredControlFlow = false;
    auto Source = emitC(Case.Program, Options);
    ASSERT_TRUE(static_cast<bool>(Source))
        << llvm::toString(Source.takeError());
    *Source = "#include <string.h>\n" + *Source +
              makeCHarness(Case.Environment, *Expected, Memory);
    compileAndRun(Backend, *Source);
    return;
  }

  RustEmitterOptions Options;
  Options.PreferStructuredControlFlow = false;
  auto Source = emitRust(Case.Program, Options);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  *Source += makeRustHarness(Case.Environment, *Expected, Memory);
  compileAndRun(Backend, *Source);
}

void addSyntheticCases(std::vector<DifferentialCase> &Cases) {
  auto Add = [&](std::string Name, auto Program,
                 ExecutionEnvironment Environment = {}) {
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    Cases.push_back(
        {std::move(Name), std::move(*Program), std::move(Environment)});
  };

  Add("v0-sign-extension",
      analyzeProgram(Version::V0, {encode(Opcode::MOV64_IMM, 0, 0, 0,
                                          std::numeric_limits<int32_t>::max()),
                                   encode(Opcode::ADD32_IMM, 0, 0, 0, 1),
                                   encode(Opcode::EXIT)}));
  Add("v2-pqr",
      analyzeProgram(Version::V2, {encode(Opcode::MOV64_IMM, 0, 0, 0, -2),
                                   encode(Opcode::UDIV64_IMM, 0, 0, 0, -1),
                                   encode(Opcode::SUB64_IMM, 0, 0, 0, 3),
                                   encode(Opcode::EXIT)}));
  Add("v3-nested-calls",
      analyzeProgram(
          Version::V3,
          {encode(Opcode::MOV64_IMM, 0, 0, 0, 1),
           encode(Opcode::CALL_IMM, 0, 1, 0, 3),
           encode(Opcode::ADD64_IMM, 0, 0, 0, 1), encode(Opcode::EXIT),
           encode(Opcode::MOV64_IMM, 0, 0, 0, 0),
           encode(Opcode::ADD64_IMM, 0, 0, 0, 40),
           encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
           encode(Opcode::ADD64_IMM, 0, 0, 0, 1), encode(Opcode::EXIT)}));

  const SyscallInfo *Log64 = getSyscallInfo(Syscall::Log64);
  ASSERT_NE(Log64, nullptr);
  ExecutionEnvironment MemoryEnvironment;
  MemoryEnvironment.Memory.push_back(
      {kInputStart, {9, 0, 0, 0, 0, 0, 0, 0}, false, "input"});
  Add("memory-and-syscall",
      analyzeProgram(
          Version::V3,
          {encode(Opcode::LD_DW_REG, 2, 1),
           encode(Opcode::ST_DW_REG, kFramePointerRegister, 2, -8),
           encode(Opcode::LD_DW_REG, 1, kFramePointerRegister, -8),
           encode(Opcode::CALL_IMM, 0, 0, 0, static_cast<int32_t>(Log64->Hash)),
           encode(Opcode::EXIT)}),
      std::move(MemoryEnvironment));
  Add("memory-fault",
      analyzeProgram(Version::V3,
                     {encode(Opcode::MOV64_IMM, 1, 0, 0, 1),
                      encode(Opcode::LD_DW_REG, 0, 1), encode(Opcode::EXIT)}));
  Add("unresolved-legacy-call",
      analyzeProgram(Version::V0, {encode(Opcode::CALL_IMM, 0, 0, 0, -1),
                                   encode(Opcode::EXIT)}));

  const auto ContinuationTarget =
      encodeLDDW(2, kBytecodeStart + kInstructionSize);
  Add("callx-continuation",
      analyzeProgram(Version::V3,
                     {ContinuationTarget[0], ContinuationTarget[1],
                      encode(Opcode::CALL_REG, 2), encode(Opcode::EXIT)}));
}

void addOfficialRelocationCase(std::vector<DifferentialCase> &Cases) {
#ifdef NEVERD_SOURCE_ROOT
  const std::filesystem::path Path = std::filesystem::path(NEVERD_SOURCE_ROOT) /
                                     "local_docs" / "sbpf" / "tests" / "elfs" /
                                     "reloc_64_relative_data_sbpfv0.so";
  if (!std::filesystem::exists(Path))
    return;
  auto Image = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  auto Program = analyze(*Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  Cases.push_back({"official-v0-relocated-data", std::move(*Program), {}});
#endif
}

void runAllSourceCases(SourceBackend Backend) {
  const llvm::StringRef CompilerName = compilerName(Backend);
  if (!llvm::sys::findProgramByName(CompilerName))
    GTEST_SKIP() << CompilerName.str() << " is not available";

  std::vector<DifferentialCase> Cases;
  addSyntheticCases(Cases);
  addOfficialRelocationCase(Cases);
  ASSERT_GE(Cases.size(), 6u);
  for (const DifferentialCase &Case : Cases)
    runDifferential(Backend, Case);
}

TEST(SBFSourceDifferential, GeneratedCMatchesTheRawOracle) {
  runAllSourceCases(SourceBackend::C);
}

TEST(SBFSourceDifferential, GeneratedRustMatchesTheRawOracle) {
  runAllSourceCases(SourceBackend::Rust);
}

} // namespace
} // namespace neverd::sbf
