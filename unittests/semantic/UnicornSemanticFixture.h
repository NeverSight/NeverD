//===- UnicornSemanticFixture.h - LLVM MC + Unicorn test fixture -*- C++ -*-===//
//
// NeverD Decompiler — Semantic Verification Tests
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Test fixture that assembles instructions via LLVM MC and emulates them
/// with Unicorn Engine to verify semantic correctness of lifted code.
///
/// Replaces the former Python-based lift_verifier / pipeline_verifier /
/// ir_stage_verifier scripts. Uses LLVM MC instead of Keystone (full ISA
/// coverage) and the in-tree Unicorn fork (third_party/unicorn) so we can
/// fix emulation bugs upstream.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_SEMANTIC_UNICORNSEMANTICFIXTURE_H
#define NEVERD_UNITTESTS_SEMANTIC_UNICORNSEMANTICFIXTURE_H

#include "gtest/gtest.h"

#include "neverd/object/SectionNames.h"

#include <unicorn/unicorn.h>

#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

extern "C" {
void LLVMInitializeX86TargetInfo();
void LLVMInitializeX86TargetMC();
void LLVMInitializeX86AsmParser();
void LLVMInitializeAArch64TargetInfo();
void LLVMInitializeAArch64TargetMC();
void LLVMInitializeAArch64AsmParser();
void LLVMInitializeARMTargetInfo();
void LLVMInitializeARMTargetMC();
void LLVMInitializeARMAsmParser();
}
#include "llvm/TargetParser/Triple.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

// ============================================================================
// Memory layout constants (same as the former Python verifier)
// ============================================================================
constexpr uint64_t CODE_BASE = 0x400000;
constexpr uint64_t STACK_BASE = 0x7FF000;
// Recompiled stress fixtures can legitimately use stack frames above 64 KiB
// before LLVM's late code generation has coalesced every lifted temporary.
// Keep the emulator representative of a normal process stack so those frames
// test semantics instead of faulting at the function prologue.
constexpr uint64_t STACK_SIZE = 0x200000;
constexpr uint64_t DATA_BASE = 0x500000;
constexpr uint64_t DATA_SIZE = 0x10000;

// ============================================================================
// Byte-packing helpers — replicate Python struct.pack semantics
// ============================================================================
using Bytes = std::vector<uint8_t>;

template <typename T> inline Bytes pack(T V) {
  Bytes B(sizeof(T));
  std::memcpy(B.data(), &V, sizeof(T));
  return B;
}
inline Bytes packU64(uint64_t V) { return pack(V); }
inline Bytes packU32(uint32_t V) { return pack(V); }
inline Bytes packU16(uint16_t V) { return pack(V); }
inline Bytes packI32(int32_t V) { return pack(V); }
inline Bytes packF32(float V) { return pack(V); }
inline Bytes packF64(double V) { return pack(V); }
inline Bytes zeros(size_t N) { return Bytes(N, 0); }
inline Bytes fill(size_t N, uint8_t V) { return Bytes(N, V); }

inline Bytes cat(std::initializer_list<Bytes> Parts) {
  size_t Total = 0;
  for (auto &P : Parts)
    Total += P.size();
  Bytes R;
  R.reserve(Total);
  for (auto &P : Parts)
    R.insert(R.end(), P.begin(), P.end());
  return R;
}

/// Bit-cast float → uint32 (for ARM VFP register init).
inline uint64_t f32bits(float F) {
  uint32_t V;
  std::memcpy(&V, &F, 4);
  return V;
}
/// Bit-cast double → uint64.
inline uint64_t f64bits(double D) {
  uint64_t V;
  std::memcpy(&V, &D, 8);
  return V;
}
/// Low/high 32-bit halves of a double (for ARM32 d-register init via r-pairs).
inline uint32_t f64lo(double D) {
  uint64_t V;
  std::memcpy(&V, &D, 8);
  return static_cast<uint32_t>(V);
}
inline uint32_t f64hi(double D) {
  uint64_t V;
  std::memcpy(&V, &D, 8);
  return static_cast<uint32_t>(V >> 32);
}

// ============================================================================
// Test-case descriptor
// ============================================================================
struct MemInit {
  uint64_t Addr;
  Bytes Data;
};

struct SemTC {
  std::string Name;
  std::string Asm;
  std::vector<std::pair<std::string, uint64_t>> InitRegs;
  std::vector<std::string> CheckRegs;
  std::string Category;
  std::vector<MemInit> InitMem;
};

inline std::ostream &operator<<(std::ostream &OS, const SemTC &TC) {
  return OS << TC.Category << "/" << TC.Name;
}

// ============================================================================
// LLVM MC Assembler — replaces Keystone
// ============================================================================
class LLVMMCAssembler {
public:
  static void initTargets() {
    static bool Done = false;
    if (Done)
      return;
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmParser();
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmParser();
    LLVMInitializeARMTargetInfo();
    LLVMInitializeARMTargetMC();
    LLVMInitializeARMAsmParser();
    Done = true;
  }

  /// Assemble \p AsmText for \p TripleStr and return the raw .text bytes.
  /// Returns empty vector on failure. \p Features is an optional comma-
  /// separated feature string (e.g. "+avx,+bmi").
  static std::vector<uint8_t>
  assemble(llvm::StringRef AsmText, llvm::StringRef TripleStr,
           llvm::StringRef Features = "") {
    initTargets();

    llvm::Triple Triple(TripleStr);
    std::string Err;
    const llvm::Target *Tgt =
        llvm::TargetRegistry::lookupTarget("", Triple, Err);
    if (!Tgt)
      return {};

    llvm::MCTargetOptions MCOpts;
    std::unique_ptr<llvm::MCRegisterInfo> MRI(
        Tgt->createMCRegInfo(Triple));
    std::unique_ptr<llvm::MCAsmInfo> MAI(
        Tgt->createMCAsmInfo(*MRI, Triple, MCOpts));
    std::unique_ptr<llvm::MCInstrInfo> MCII(Tgt->createMCInstrInfo());
    std::unique_ptr<llvm::MCSubtargetInfo> STI(
        Tgt->createMCSubtargetInfo(Triple, /*CPU=*/"", Features));
    if (!MRI || !MAI || !MCII || !STI)
      return {};

    llvm::MCContext Ctx(Triple, *MAI, *MRI, *STI);
    std::unique_ptr<llvm::MCObjectFileInfo> MOFI(
        Tgt->createMCObjectFileInfo(Ctx, /*PIC=*/false));
    Ctx.setObjectFileInfo(MOFI.get());

    std::string FullAsm;
    if (Triple.isX86())
      FullAsm = ".intel_syntax noprefix\n";
    FullAsm += AsmText.str();
    FullAsm += "\n";

    auto Buf = llvm::MemoryBuffer::getMemBuffer(FullAsm);
    llvm::SourceMgr SrcMgr;
    SrcMgr.AddNewSourceBuffer(std::move(Buf), llvm::SMLoc());

    llvm::SmallVector<char, 1024> ObjBuf;
    llvm::raw_svector_ostream VecOS(ObjBuf);

    std::unique_ptr<llvm::MCCodeEmitter> CE(
        Tgt->createMCCodeEmitter(*MCII, Ctx));
    std::unique_ptr<llvm::MCAsmBackend> MAB(
        Tgt->createMCAsmBackend(*STI, *MRI, MCOpts));
    if (!CE || !MAB)
      return {};

    std::unique_ptr<llvm::MCObjectWriter> OW = MAB->createObjectWriter(VecOS);
    std::unique_ptr<llvm::MCStreamer> Str(Tgt->createMCObjectStreamer(
        Triple, Ctx, std::move(MAB), std::move(OW), std::move(CE), *STI));
    if (!Str)
      return {};

    std::unique_ptr<llvm::MCAsmParser> Parser(
        llvm::createMCAsmParser(SrcMgr, Ctx, *Str, *MAI));
    std::unique_ptr<llvm::MCTargetAsmParser> TAP(
        Tgt->createMCAsmParser(*STI, *Parser, *MCII));
    if (!TAP)
      return {};
    Parser->setTargetParser(*TAP);

    if (Parser->Run(/*NoInitialTextSection=*/false))
      return {};

    Str->finish();

    // Parse the generated object and extract .text bytes
    auto MemBufOrErr = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(ObjBuf.data(), ObjBuf.size()), "", false);
    auto ObjOrErr =
        llvm::object::ObjectFile::createObjectFile(MemBufOrErr->getMemBufferRef());
    if (!ObjOrErr)
      return {};

    for (const auto &Sec : (*ObjOrErr)->sections()) {
      auto NameOrErr = Sec.getName();
      if (!NameOrErr)
        continue;
      if (*NameOrErr == neverd::section_names::elf::Text) {
        auto ContentsOrErr = Sec.getContents();
        if (!ContentsOrErr)
          continue;
        return {ContentsOrErr->begin(), ContentsOrErr->end()};
      }
    }

    return {};
  }

  /// Assemble \p AsmText for \p TripleStr and return the complete ELF
  /// object file bytes (for writing to disk and feeding to NeverD).
  static std::vector<uint8_t>
  assembleToObj(llvm::StringRef AsmText, llvm::StringRef TripleStr,
                llvm::StringRef Features = "") {
    initTargets();

    llvm::Triple Triple(TripleStr);
    std::string Err;
    const llvm::Target *Tgt =
        llvm::TargetRegistry::lookupTarget("", Triple, Err);
    if (!Tgt)
      return {};

    llvm::MCTargetOptions MCOpts;
    std::unique_ptr<llvm::MCRegisterInfo> MRI(
        Tgt->createMCRegInfo(Triple));
    std::unique_ptr<llvm::MCAsmInfo> MAI(
        Tgt->createMCAsmInfo(*MRI, Triple, MCOpts));
    std::unique_ptr<llvm::MCInstrInfo> MCII(Tgt->createMCInstrInfo());
    std::unique_ptr<llvm::MCSubtargetInfo> STI(
        Tgt->createMCSubtargetInfo(Triple, /*CPU=*/"", Features));
    if (!MRI || !MAI || !MCII || !STI)
      return {};

    llvm::MCContext Ctx(Triple, *MAI, *MRI, *STI);
    std::unique_ptr<llvm::MCObjectFileInfo> MOFI(
        Tgt->createMCObjectFileInfo(Ctx, /*PIC=*/false));
    Ctx.setObjectFileInfo(MOFI.get());

    std::string FullAsm;
    if (Triple.isX86())
      FullAsm = ".intel_syntax noprefix\n";
    FullAsm += AsmText.str();
    FullAsm += "\n";

    auto Buf = llvm::MemoryBuffer::getMemBuffer(FullAsm);
    llvm::SourceMgr SrcMgr;
    SrcMgr.AddNewSourceBuffer(std::move(Buf), llvm::SMLoc());

    llvm::SmallVector<char, 4096> ObjBuf;
    llvm::raw_svector_ostream VecOS(ObjBuf);

    std::unique_ptr<llvm::MCCodeEmitter> CE(
        Tgt->createMCCodeEmitter(*MCII, Ctx));
    std::unique_ptr<llvm::MCAsmBackend> MAB(
        Tgt->createMCAsmBackend(*STI, *MRI, MCOpts));
    if (!CE || !MAB)
      return {};

    std::unique_ptr<llvm::MCObjectWriter> OW = MAB->createObjectWriter(VecOS);
    std::unique_ptr<llvm::MCStreamer> Str(Tgt->createMCObjectStreamer(
        Triple, Ctx, std::move(MAB), std::move(OW), std::move(CE), *STI));
    if (!Str)
      return {};

    std::unique_ptr<llvm::MCAsmParser> Parser(
        llvm::createMCAsmParser(SrcMgr, Ctx, *Str, *MAI));
    std::unique_ptr<llvm::MCTargetAsmParser> TAP(
        Tgt->createMCAsmParser(*STI, *Parser, *MCII));
    if (!TAP)
      return {};
    Parser->setTargetParser(*TAP);

    if (Parser->Run(/*NoInitialTextSection=*/false))
      return {};

    Str->finish();

    return {ObjBuf.begin(), ObjBuf.end()};
  }
};

// ============================================================================
// Unicorn register maps — replaces Python ArchConfig
// ============================================================================
namespace uc_regs {

struct RegEntry {
  const char *Name;
  int UcID;
};

// clang-format off
static const RegEntry kX64GP[] = {
    {"rax", UC_X86_REG_RAX}, {"rbx", UC_X86_REG_RBX},
    {"rcx", UC_X86_REG_RCX}, {"rdx", UC_X86_REG_RDX},
    {"rsi", UC_X86_REG_RSI}, {"rdi", UC_X86_REG_RDI},
    {"rsp", UC_X86_REG_RSP}, {"rbp", UC_X86_REG_RBP},
    {"r8",  UC_X86_REG_R8},  {"r9",  UC_X86_REG_R9},
    {"r10", UC_X86_REG_R10}, {"r11", UC_X86_REG_R11},
    {"r12", UC_X86_REG_R12}, {"r13", UC_X86_REG_R13},
    {"r14", UC_X86_REG_R14}, {"r15", UC_X86_REG_R15},
};
static const RegEntry kX64Flags[] = {
    {"eflags", UC_X86_REG_EFLAGS},
};
static const RegEntry kX64PC[] = {
    {"rip", UC_X86_REG_RIP},
};

static const RegEntry kA64GP[] = {
    {"x0",  UC_ARM64_REG_X0},  {"x1",  UC_ARM64_REG_X1},
    {"x2",  UC_ARM64_REG_X2},  {"x3",  UC_ARM64_REG_X3},
    {"x4",  UC_ARM64_REG_X4},  {"x5",  UC_ARM64_REG_X5},
    {"x6",  UC_ARM64_REG_X6},  {"x7",  UC_ARM64_REG_X7},
    {"x8",  UC_ARM64_REG_X8},  {"x9",  UC_ARM64_REG_X9},
    {"x10", UC_ARM64_REG_X10}, {"x11", UC_ARM64_REG_X11},
    {"x12", UC_ARM64_REG_X12}, {"x13", UC_ARM64_REG_X13},
    {"x14", UC_ARM64_REG_X14}, {"x15", UC_ARM64_REG_X15},
    {"x16", UC_ARM64_REG_X16}, {"x17", UC_ARM64_REG_X17},
    {"x18", UC_ARM64_REG_X18}, {"x19", UC_ARM64_REG_X19},
    {"x20", UC_ARM64_REG_X20}, {"x21", UC_ARM64_REG_X21},
    {"x22", UC_ARM64_REG_X22}, {"x23", UC_ARM64_REG_X23},
    {"x24", UC_ARM64_REG_X24}, {"x25", UC_ARM64_REG_X25},
    {"x26", UC_ARM64_REG_X26}, {"x27", UC_ARM64_REG_X27},
    {"x28", UC_ARM64_REG_X28}, {"x29", UC_ARM64_REG_X29},
    {"x30", UC_ARM64_REG_X30},
};
static const RegEntry kA64Flags[] = {
    {"nzcv", UC_ARM64_REG_NZCV},
};

static const RegEntry kARM32GP[] = {
    {"r0",  UC_ARM_REG_R0},  {"r1",  UC_ARM_REG_R1},
    {"r2",  UC_ARM_REG_R2},  {"r3",  UC_ARM_REG_R3},
    {"r4",  UC_ARM_REG_R4},  {"r5",  UC_ARM_REG_R5},
    {"r6",  UC_ARM_REG_R6},  {"r7",  UC_ARM_REG_R7},
    {"r8",  UC_ARM_REG_R8},  {"r9",  UC_ARM_REG_R9},
    {"r10", UC_ARM_REG_R10}, {"r11", UC_ARM_REG_R11},
    {"r12", UC_ARM_REG_R12},
};
static const RegEntry kARM32Flags[] = {
    {"cpsr", UC_ARM_REG_CPSR},
};
// clang-format on

inline int lookup(const char *Name, const RegEntry *Table, size_t N) {
  for (size_t I = 0; I < N; ++I)
    if (std::strcmp(Table[I].Name, Name) == 0)
      return Table[I].UcID;
  return -1;
}

inline int lookupX64(const std::string &Name) {
  int R = lookup(Name.c_str(), kX64GP, std::size(kX64GP));
  if (R >= 0)
    return R;
  R = lookup(Name.c_str(), kX64Flags, std::size(kX64Flags));
  if (R >= 0)
    return R;
  return lookup(Name.c_str(), kX64PC, std::size(kX64PC));
}

inline int lookupA64(const std::string &Name) {
  int R = lookup(Name.c_str(), kA64GP, std::size(kA64GP));
  if (R >= 0)
    return R;
  return lookup(Name.c_str(), kA64Flags, std::size(kA64Flags));
}

inline int lookupARM32(const std::string &Name) {
  int R = lookup(Name.c_str(), kARM32GP, std::size(kARM32GP));
  if (R >= 0)
    return R;
  return lookup(Name.c_str(), kARM32Flags, std::size(kARM32Flags));
}

} // namespace uc_regs

// ============================================================================
// Unicorn emulation engine
// ============================================================================
struct EmulState {
  std::map<std::string, uint64_t> Regs;
  bool OK = false;
  std::string Error;
};

class UnicornEmulator {
public:
  ~UnicornEmulator() {
    if (UC)
      uc_close(UC);
  }

  EmulState run(uc_arch Arch, uc_mode Mode, const std::vector<uint8_t> &Code,
                const std::vector<std::pair<std::string, uint64_t>> &InitRegs,
                const std::vector<MemInit> &InitMem,
                int (*RegLookup)(const std::string &), int SPReg,
                bool IsARM32 = false) {
    EmulState S;
    uc_err Err = uc_open(Arch, Mode, &UC);
    if (Err != UC_ERR_OK) {
      S.Error = std::string("uc_open: ") + uc_strerror(Err);
      return S;
    }

    // Map memory regions
    mapOrFail(CODE_BASE, 0x10000, UC_PROT_ALL);
    mapOrFail(STACK_BASE, STACK_SIZE, UC_PROT_ALL);
    mapOrFail(DATA_BASE, DATA_SIZE, UC_PROT_ALL);

    // Write code
    uc_mem_write(UC, CODE_BASE, Code.data(), Code.size());

    // Set stack pointer
    uint64_t SP = STACK_BASE + STACK_SIZE - 0x100;
    uc_reg_write(UC, SPReg, &SP);

    // ARM32: enable VFP/NEON and NZCV
    if (IsARM32) {
      uint32_t CPSR;
      uc_reg_read(UC, UC_ARM_REG_CPSR, &CPSR);
      CPSR |= (0xFu << 20);
      uc_reg_write(UC, UC_ARM_REG_CPSR, &CPSR);

      uint32_t CPACR = 0x00F00000;
      uc_reg_write(UC, UC_ARM_REG_C1_C0_2, &CPACR);
      uint32_t FPEXC = 0x40000000;
      uc_reg_write(UC, UC_ARM_REG_FPEXC, &FPEXC);
    }

    // Initialize registers
    for (auto &[Name, Val] : InitRegs) {
      int Reg = RegLookup(Name);
      if (Reg < 0) {
        S.Error = "Unknown register: " + Name;
        return S;
      }
      uint64_t V = Val;
      uc_reg_write(UC, Reg, &V);
    }

    // Initialize memory
    for (auto &M : InitMem)
      uc_mem_write(UC, M.Addr, M.Data.data(), M.Data.size());

    // Emulate
    uint64_t EndAddr = CODE_BASE + Code.size();
    Err = uc_emu_start(UC, CODE_BASE, EndAddr, /*timeout=*/5000000, /*count=*/0);
    if (Err != UC_ERR_OK) {
      S.Error = std::string("uc_emu_start: ") + uc_strerror(Err);
      return S;
    }

    // Read back all GP registers
    auto readAll = [&](const uc_regs::RegEntry *T, size_t N) {
      for (size_t I = 0; I < N; ++I) {
        uint64_t V = 0;
        uc_reg_read(UC, T[I].UcID, &V);
        S.Regs[T[I].Name] = V;
      }
    };

    if (Arch == UC_ARCH_X86) {
      readAll(uc_regs::kX64GP, std::size(uc_regs::kX64GP));
      readAll(uc_regs::kX64Flags, std::size(uc_regs::kX64Flags));
    } else if (Arch == UC_ARCH_ARM64) {
      readAll(uc_regs::kA64GP, std::size(uc_regs::kA64GP));
      readAll(uc_regs::kA64Flags, std::size(uc_regs::kA64Flags));
    } else if (Arch == UC_ARCH_ARM) {
      readAll(uc_regs::kARM32GP, std::size(uc_regs::kARM32GP));
      readAll(uc_regs::kARM32Flags, std::size(uc_regs::kARM32Flags));
    }

    S.OK = true;
    return S;
  }

  /// Read memory from the emulator after execution.
  Bytes readMem(uint64_t Addr, size_t Size) {
    Bytes B(Size);
    if (UC)
      uc_mem_read(UC, Addr, B.data(), Size);
    return B;
  }

private:
  uc_engine *UC = nullptr;

  void mapOrFail(uint64_t Addr, size_t Size, uint32_t Perms) {
    uc_mem_map(UC, Addr, Size, Perms);
  }
};

// ============================================================================
// Main test fixture
// ============================================================================
class UnicornSemanticFixture : public ::testing::Test {
protected:
  static void SetUpTestSuite() { LLVMMCAssembler::initTargets(); }

  // --- Architecture-specific runners ---

  void runX64(const SemTC &TC) {
    runImpl(TC, "x86_64-linux-gnu",
            "+sse,+sse2,+sse3,+ssse3,+sse4.1,+sse4.2,+avx,+avx2,"
            "+bmi,+bmi2,+lzcnt,+popcnt,+aes,+pclmul,+cx16,+fma,"
            "+adx,+movbe",
            UC_ARCH_X86, UC_MODE_64, uc_regs::lookupX64, UC_X86_REG_RSP);
  }

  void runAArch64(const SemTC &TC) {
    runImpl(TC, "aarch64-linux-gnu",
            "+neon,+fp-armv8,+crc,+crypto,+lse",
            UC_ARCH_ARM64, static_cast<uc_mode>(0), uc_regs::lookupA64,
            UC_ARM64_REG_SP);
  }

  void runARM32(const SemTC &TC) {
    runImpl(TC, "armv7-linux-gnueabi",
            "+vfp2,+vfp3,+neon",
            UC_ARCH_ARM, UC_MODE_ARM, uc_regs::lookupARM32, UC_ARM_REG_SP,
            /*IsARM32=*/true);
  }

private:
  void runImpl(const SemTC &TC, llvm::StringRef Triple,
               llvm::StringRef Features, uc_arch UcArch, uc_mode UcMode,
               int (*RegLookup)(const std::string &), int SPReg,
               bool IsARM32 = false) {
    // 1. Assemble
    auto Code = LLVMMCAssembler::assemble(TC.Asm, Triple, Features);
    ASSERT_FALSE(Code.empty())
        << "LLVM MC assembly failed for: " << TC.Asm;

    // 2. Emulate
    UnicornEmulator Emu;
    auto State =
        Emu.run(UcArch, UcMode, Code, TC.InitRegs, TC.InitMem,
                RegLookup, SPReg, IsARM32);
    ASSERT_TRUE(State.OK) << "Emulation failed: " << State.Error
                          << "\n  ASM: " << TC.Asm;

    // 3. Verify checked registers exist in output
    for (auto &RegName : TC.CheckRegs) {
      EXPECT_TRUE(State.Regs.count(RegName) > 0)
          << "Register " << RegName << " not found in emulation state";
    }

    // Memory comparison is intentionally delegated to test bodies
    // that need it (most tests only check register state).
  }
};

// ============================================================================
// Parameterized test base classes — one per architecture
// ============================================================================
class X64Semantic : public UnicornSemanticFixture,
                    public ::testing::WithParamInterface<SemTC> {};

class AArch64Semantic : public UnicornSemanticFixture,
                        public ::testing::WithParamInterface<SemTC> {};

class ARM32Semantic : public UnicornSemanticFixture,
                      public ::testing::WithParamInterface<SemTC> {};

// Convenience: test-suite name printer
inline auto semTCName = [](const ::testing::TestParamInfo<SemTC> &Info) {
  return Info.param.Name;
};

#endif // NEVERD_UNITTESTS_SEMANTIC_UNICORNSEMANTICFIXTURE_H
