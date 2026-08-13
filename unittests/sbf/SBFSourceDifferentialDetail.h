//===- SBFSourceDifferentialDetail.h - Source differential fixtures ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The programs, the oracle memory image and the small utilities both
/// generated-source harnesses are written against.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_SBF_SBFSOURCEDIFFERENTIALDETAIL_H
#define NEVERD_UNITTESTS_SBF_SBFSOURCEDIFFERENTIALDETAIL_H

#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFAnalyzer.h"
#include "neverd/sbf/runtime/SBFInterpreter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace neverd::sbf::test {

inline constexpr unsigned kBitsPerByte = 8;
inline constexpr unsigned kRuntimeTraceCapacity = 16;
inline constexpr uint64_t kHashOffset = UINT64_C(0xcbf29ce484222325);
inline constexpr uint64_t kHashPrime = UINT64_C(0x100000001b3);

using EncodedInstruction = std::array<uint8_t, kInstructionSize>;

enum class SourceBackend : uint8_t { C, Rust };

inline llvm::StringRef compilerName(SourceBackend Backend) {
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

inline EncodedInstruction encode(Opcode ID, uint8_t Dst = 0, uint8_t Src = 0,
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

inline std::array<EncodedInstruction, kLDDWSlotCount>
encodeLDDW(uint8_t Dst, uint64_t Immediate) {
  std::array<EncodedInstruction, 2> Result{
      encode(Opcode::LDDW, Dst, 0, 0, static_cast<int32_t>(Immediate)), {}};
  llvm::support::endian::write32le(
      Result[1].data() + kImmediateOffset,
      static_cast<uint32_t>(Immediate >>
                            std::numeric_limits<uint32_t>::digits));
  return Result;
}

inline llvm::Expected<SBFProgram>
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

inline std::vector<MemoryRegion>
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

inline uint64_t hashWritableMemory(const std::vector<MemoryRegion> &Memory) {
  uint64_t Hash = kHashOffset;
  for (const MemoryRegion &Region : Memory)
    if (Region.Writable)
      for (uint8_t Byte : Region.Bytes) {
        Hash ^= Byte;
        Hash *= kHashPrime;
      }
  return Hash;
}

inline std::string hexWord(uint64_t Value) {
  return "0x" + llvm::utohexstr(Value) + "u64";
}

inline bool isZeroFilled(const MemoryRegion &Region) {
  return std::all_of(Region.Bytes.begin(), Region.Bytes.end(),
                     [](uint8_t Byte) { return Byte == 0; });
}

} // namespace neverd::sbf::test

#endif // NEVERD_UNITTESTS_SBF_SBFSOURCEDIFFERENTIALDETAIL_H
