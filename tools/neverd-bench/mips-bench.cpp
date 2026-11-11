//===- mips-bench.cpp - Bounded MIPS decode/lift measurements -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare physical decoding with matching Capstone options, and separately
/// measure decode plus LowIR lifting. Input is an explicitly selected raw code
/// span; unsupported or truncated instructions fail instead of being skipped.

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"

#include "llvm/Support/MemoryBuffer.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

using namespace neverd;

namespace {
enum class Stage {
#define NEVERD_MIPS_BENCH_STAGE(Name, Spelling) Name,
#include "mips-bench.def"
};

struct Capstone {
  csh Handle = 0;
  cs_insn *Instruction = nullptr;
  ~Capstone() {
    if (Instruction)
      cs_free(Instruction, 1);
    if (Handle)
      cs_close(&Handle);
  }
};

struct Result {
  uint64_t Instructions = 0;
  uint64_t Operations = 0;
  uint64_t Digest = 0;
  void mix(uint64_t Value) { Digest = (Digest ^ Value) * 1099511628211ULL; }
  void variable(const NdVar &V) {
    mix(unsigned(V.Space));
    mix(V.Offset);
    mix(V.Size);
  }
};
} // namespace

int main(int Argc, char **Argv) try {
  if (Argc != 5)
    throw std::invalid_argument("usage: neverd-mips-bench PROFILE "
                                "capstone|decode|lift RAW_CODE REPEATS");
  Arch Target = Arch::Unknown;
  InstructionMode Encoding = InstructionMode::Default;
  bool DSP = false, DSPR2 = false, MSA = false, MIPS16E2 = false;
  unsigned Revision = 2;
#define NEVERD_MIPS_BENCH_PROFILE(Name, ArchName, Mode, Rev, Dsp, Dspr2, Msa,  \
                                  E2)                                          \
  if (std::string_view(Argv[1]) == #Name) {                                    \
    Target = Arch::ArchName;                                                   \
    Encoding = InstructionMode::Mode;                                          \
    Revision = Rev;                                                            \
    DSP = Dsp;                                                                 \
    DSPR2 = Dspr2;                                                             \
    MSA = Msa;                                                                 \
    MIPS16E2 = E2;                                                             \
  }
#include "mips-bench.def"
  if (!isMIPS(Target))
    throw std::invalid_argument("unsupported MIPS benchmark profile");
  std::optional<Stage> Selected;
#define NEVERD_MIPS_BENCH_STAGE(Name, Spelling)                                \
  if (std::string_view(Argv[2]) == Spelling)                                   \
    Selected = Stage::Name;
#include "mips-bench.def"
  if (!Selected)
    throw std::invalid_argument("unsupported measurement stage");
  uint64_t Repeats = 0;
  const std::string_view Number(Argv[4]);
  const auto Parsed =
      std::from_chars(Number.data(), Number.data() + Number.size(), Repeats);
  if (Parsed.ec != std::errc() || Parsed.ptr != Number.data() + Number.size() ||
      !Repeats)
    throw std::invalid_argument("REPEATS must be a positive integer");
  auto Input = llvm::MemoryBuffer::getFile(Argv[3], false, false);
  if (!Input)
    throw std::runtime_error(Input.getError().message());
  const auto Bytes = (*Input)->getBuffer();
  if (Bytes.empty() || Bytes.size() % (isMIPSCompressedMode(Encoding) ? 2 : 4))
    throw std::invalid_argument("expected a nonempty aligned MIPS code span");
  Decoder Decode;
  BinaryImage Profile;
  Profile.Arch = Target;
  Profile.Bits = isMIPS64(Target) ? Bitness::Bits64 : Bitness::Bits32;
  Profile.Mode = Encoding;
  Profile.MIPS.emplace();
  Profile.MIPS->ABI = isMIPS64(Target) ? MIPSABI::N64 : MIPSABI::O32;
  Profile.MIPS->HasDSP = DSP;
  Profile.MIPS->HasDSPR2 = DSPR2;
  Profile.MIPS->HasMIPS16E2 = MIPS16E2;
  Profile.MIPS->HasMSA = MSA;
  Profile.MIPS->ISARevision = Revision;
  Profile.MIPS->HasMicroMIPS = Encoding == InstructionMode::MicroMIPS;
  Profile.MIPS->HasMIPS16 = Encoding == InstructionMode::MIPS16;
  if (!Decode.init(Profile))
    throw std::runtime_error("decoder initialization failed");
  Decode.setDetail(true);
  Decode.setText(false);
  Capstone Reference;
  const auto Mode = static_cast<cs_mode>(
      (Revision == 6 ? (isMIPS64(Target) ? CS_MODE_MIPS64R6 : CS_MODE_MIPS32R6)
       : isMIPS64(Target) ? CS_MODE_MIPS64R2
       : MSA              ? CS_MODE_MIPS32R5
                          : CS_MODE_MIPS32R2) |
      (Revision == 6 && isMIPS64(Target) ? CS_MODE_MIPS_PTR64 : 0) |
      (Encoding == InstructionMode::MicroMIPS ? CS_MODE_MICRO
       : Encoding == InstructionMode::MIPS16  ? CS_MODE_MIPS16
                                              : 0) |
      (archLittleEndian(Target) ? CS_MODE_LITTLE_ENDIAN : CS_MODE_BIG_ENDIAN));
  if (cs_open(CS_ARCH_MIPS, Mode, &Reference.Handle) != CS_ERR_OK ||
      cs_option(Reference.Handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK ||
      cs_option(Reference.Handle, CS_OPT_TEXT, CS_OPT_OFF) != CS_ERR_OK ||
      !(Reference.Instruction = cs_malloc(Reference.Handle)))
    throw std::runtime_error("Capstone initialization failed");
  std::vector<LowOp> Ops;
  const auto Run = [&](bool Fingerprint) {
    Result R;
    for (size_t Offset = 0; Offset < Bytes.size();) {
      const auto *Code =
          reinterpret_cast<const uint8_t *>(Bytes.data()) + Offset;
      const size_t Available = Bytes.size() - Offset;
      size_t Remaining = Available;
      uint64_t Address = 0x10000 + Offset;
      unsigned Size = 0;
      unsigned Count = 1;
      if (*Selected == Stage::Capstone) {
        Reference.Instruction->illegal = false;
        if (!cs_disasm_iter(Reference.Handle, &Code, &Remaining, &Address,
                            Reference.Instruction))
          throw std::runtime_error("Capstone rejected input");
        Size = Reference.Instruction->size;
        if (Fingerprint)
          R.mix(Reference.Instruction->id);
      } else if (*Selected == Stage::Decode) {
        DecodedInsn Instruction{};
        Size = Decode.decodeOneForLift(Code, Remaining, Address, Instruction);
        if (Fingerprint)
          R.mix(Instruction.Id);
      } else {
        DecodedExecutionUnit Unit;
        Size = Decode.decodeUnitForLift(Code, Remaining, Address, Unit);
        if (Size) {
          Ops.clear();
          Decode.liftUnitToLow(Unit, Ops);
          Count += Unit.HasDelaySlot;
          R.Operations += Ops.size();
          if (Fingerprint)
            for (const auto &Op : Ops) {
              R.mix(unsigned(Op.Opcode));
              R.variable(Op.Output);
              R.mix(Op.NumInputs);
              for (unsigned I = 0; I < Op.NumInputs; ++I)
                R.variable(Op.Inputs[I]);
            }
        }
      }
      if (!Size || Size > Available)
        throw std::runtime_error("invalid/truncated instruction span");
      R.Instructions += Count;
      if (Fingerprint) {
        R.mix(Offset);
        R.mix(Size);
      }
      Offset += Size;
    }
    return R;
  };
  const auto Expected = Run(true); // Validate and warm before timing.
  const auto Start = std::chrono::steady_clock::now();
  const auto CPUStart = std::clock();
  for (uint64_t I = 0; I < Repeats; ++I) {
    const auto Actual = Run(false);
    if (Actual.Instructions != Expected.Instructions ||
        Actual.Operations != Expected.Operations)
      throw std::runtime_error("nondeterministic measurement output");
  }
  const double CPU = double(std::clock() - CPUStart) / CLOCKS_PER_SEC;
  const double Wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - Start)
          .count();
  if (Run(true).Digest != Expected.Digest)
    throw std::runtime_error("nondeterministic measurement fingerprint");
  std::printf("{\"cpu_seconds\":%.9f,\"wall_seconds\":%.9f,"
              "\"repeats\":%llu,\"instructions_per_repeat\":%llu,"
              "\"operations_per_repeat\":%llu,\"digest\":\"%016llx\"}\n",
              CPU, Wall, (unsigned long long)Repeats,
              (unsigned long long)Expected.Instructions,
              (unsigned long long)Expected.Operations,
              (unsigned long long)Expected.Digest);
  return 0;
} catch (const std::exception &Error) {
  std::fprintf(stderr, "%s\n", Error.what());
  return 1;
}
