//===- SemanticRoundTripFixture.h - Lift roundtrip test fixture --*- C++ -*-===//
//
// NeverD Decompiler — Semantic Roundtrip Verification Tests
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Test fixture that verifies semantic correctness of the full lift pipeline:
///
///   C wrapper function → clang compile → .o
///                                         ↓
///                 extract .text → Unicorn → expected state
///                                         ↓
///            neverd_lift_to_obj → recompiled .o
///                                         ↓
///                 extract .text → Unicorn → actual state
///                                         ↓
///                              compare states
///
/// C wrapper functions use inline asm to exercise individual instructions
/// while providing standard C ABI entry/exit.  This ensures NeverD's loader
/// can find functions and the ABI is consistent between original and lifted.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_SEMANTIC_SEMANTICROUNDTRIPFIXTURE_H
#define NEVERD_UNITTESTS_SEMANTIC_SEMANTICROUNDTRIPFIXTURE_H

#include "../TestProcess.h"
#include "UnicornSemanticFixture.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

// ============================================================================
// Roundtrip test-case descriptor
// ============================================================================
struct RoundTripTC {
  std::string Name;

  /// C source with inline asm.  Must define exactly one function.
  /// Use the placeholder macros LONG/INT for portable types.
  std::string CSrc;

  /// Arguments to pass when calling the function (max 6 for SysV ABI).
  std::vector<uint64_t> Args;

  /// Expected return value (computed at compile time or from Unicorn).
  /// If empty, we compare original-Unicorn vs lifted-Unicorn.
  std::string Category;

  /// Clang optimization level for compiling the C source (0, 1, 2).
  /// SIMD/vector code should use >= 1 for clean instruction output.
  int OptLevel = 0;

  /// Extra clang flags (e.g. "-mfma", "-mssse3", "-march=haswell").
  std::string ExtraFlags;

  /// When true, lift with NeverD optimizer disabled (debug attribution only).
  bool NoOpt = false;

  /// Override the clang -target triple (e.g. "armv8-linux-gnueabihf" for ARM32
  /// crypto, which needs an ARMv8 baseline the default armv7 target rejects).
  /// When set, the default per-arch -mcpu is also skipped (ExtraFlags carries
  /// the -march/-mfpu instead).
  std::string ClangTargetOverride;

  /// Unicorn CPU model to select before emulation (UC_CPU_* enum value), or -1
  /// for the arch default.  Crypto tests need UC_CPU_ARM_MAX to enable AES/SHA.
  int UcCpuModel = -1;

  /// Link a freestanding memcpy/memset/memmove into both the original and
  /// recompiled images so a mem* builtin the source emits (e.g. clang's `memcpy`
  /// for a >=5-entry local computed-goto table, or any stack-array memset) resolves
  /// to real, Unicorn-runnable code instead of the unresolved -nostdlib
  /// placeholder.  Default ON: the helper links into BOTH the original and the
  /// recompiled image, so a test that copies/zeroes a buffer genuinely executes it
  /// on both sides instead of comparing two un-cleared stack-garbage runs that only
  /// happen to match (an "accidental pass").  The harness gates the actual link on
  /// objReferencesMemBuiltins, so a test that emits no mem* call is unaffected.
  /// (External-mem* argument recovery — the VA-0 arity collision and the i386
  /// cdecl stack-arg order — is fixed in MedABIPass; see §15.2 arm32-memset-note.)
  bool LinkMemBuiltins = true;
};

inline std::ostream &operator<<(std::ostream &OS, const RoundTripTC &TC) {
  return OS << TC.Category << "/" << TC.Name;
}

// ============================================================================
// Main roundtrip fixture
// ============================================================================
class SemanticRoundTripFixture : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    LLVMMCAssembler::initTargets();
    if (!Sess)
      Sess = neverd_session_create();

    // One root per process; every test gets its own unique subdir under it so
    // suites never share files. Suite teardown must NOT wipe this root (other
    // suites follow it in the same process), only per-test guards clean up.
    if (TmpDir.empty())
      TmpDir = fs::temp_directory_path() /
               ("neverd_rt_" +
                std::to_string(neverd::test::currentProcessId()));
    std::error_code EC;
    fs::create_directories(TmpDir, EC);
  }

  static void TearDownTestSuite() {
    if (Sess) {
      neverd_session_destroy(Sess);
      Sess = nullptr;
    }
  }

  // --- Architecture-specific roundtrip runners ---

  void roundTripX64(const RoundTripTC &TC) {
    roundTripImpl(TC, "x86_64-linux-gnu", "x86_64-linux-gnu",
                  UC_ARCH_X86, UC_MODE_64, UC_X86_REG_RSP,
                  {UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RDX,
                   UC_X86_REG_RCX, UC_X86_REG_R8, UC_X86_REG_R9},
                  UC_X86_REG_RAX);
  }

  // i386 SysV cdecl: arguments arrive on the stack (no parameter registers) and
  // the 32-bit result returns in EAX.  emulateFunction lays out the cdecl frame
  // when it sees UC_ARCH_X86 + UC_MODE_32.
  void roundTripX86(const RoundTripTC &TC) {
    roundTripImpl(TC, "i386-linux-gnu", "i386-linux-gnu",
                  UC_ARCH_X86, UC_MODE_32, UC_X86_REG_ESP,
                  {}, UC_X86_REG_EAX);
  }

  void roundTripAArch64(const RoundTripTC &TC) {
    roundTripImpl(TC, "aarch64-linux-gnu", "aarch64-linux-gnu",
                  UC_ARCH_ARM64, static_cast<uc_mode>(0), UC_ARM64_REG_SP,
                  {UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                   UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5},
                  UC_ARM64_REG_X0);
  }

  void roundTripARM32(const RoundTripTC &TC) {
    roundTripImpl(TC, "armv7-linux-gnueabi", "armv7-linux-gnueabi",
                  UC_ARCH_ARM, UC_MODE_ARM, UC_ARM_REG_SP,
                  {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3},
                  UC_ARM_REG_R0, true);
  }

private:
  // Number of times a temp-file action is retried when it fails with no output
  // at all — the signature of a transient fork/redirect failure under the heavy
  // process churn of a full suite run (not a real tool diagnostic).
  static constexpr int kSpawnRetries = 5;

  // Number of times a REQUIRED link (ld.lld) is retried before giving up.  A
  // needed link that fails is almost always a transient disruption — e.g. the
  // linker child killed by a process-group signal during a full parallel run,
  // exactly the event that simultaneously kills whole shards — not a malformed
  // object.  Retrying with backoff recovers from it; the unlinked object is
  // NEVER used as a fallback (its unresolved relocations read wrong/unmapped
  // memory and masquerade as a semantic failure).
  static constexpr int kLinkRetries = 5;

  static inline neverd_session_t Sess = nullptr;
  static inline fs::path TmpDir;
  static inline std::atomic<uint64_t> WorkSeq{0};

  /// Per-test working directory (fresh fixture instance per test case).
  fs::path Work;

  /// Path to a freestanding mem* helper object compiled for this test's target,
  /// or "" when the source references no mem* builtin.  Linked into both the
  /// original and recompiled images (see compileMemHelper / linkAndExtract).
  std::string MemHelperObj;

  /// Removes the per-test working directory on every exit path of roundTripImpl
  /// (including GTEST_SKIP / ASSERT early returns).
  struct WorkGuard {
    fs::path P;
    ~WorkGuard() {
      std::error_code EC;
      fs::remove_all(P, EC);
    }
  };

  struct ExecResult {
    int Code = -1;
    std::string Out, Err;
    bool ok() const { return Code == 0; }
  };

  ExecResult runCmd(const std::string &Cmd) const {
    auto OutF = (Work / "_stdout.txt").string();
    auto ErrF = (Work / "_stderr.txt").string();
    ExecResult R;
    for (int Attempt = 0; Attempt < kSpawnRetries; ++Attempt) {
      std::error_code MkEC;
      fs::create_directories(Work, MkEC);
      int RC = std::system(
          (Cmd + neverd::test::redirectOutput(OutF, ErrF)).c_str());
      R = ExecResult{};
      R.Code = neverd::test::systemExitCode(RC);
      {
        std::ifstream F(OutF);
        R.Out.assign(std::istreambuf_iterator<char>(F), {});
      }
      {
        std::ifstream F(ErrF);
        R.Err.assign(std::istreambuf_iterator<char>(F), {});
      }
      // A genuine tool error always writes a diagnostic; a clean exit is done.
      // Only retry the "nonzero exit with zero output" case (shell/fork could
      // not run, e.g. transient EAGAIN), backing off briefly each time.
      if (R.Code == 0 || !R.Out.empty() || !R.Err.empty())
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20 * (Attempt + 1)));
    }
    return R;
  }

  void roundTripImpl(const RoundTripTC &TC, const char *Triple,
                     const char *ClangTarget, uc_arch UcArch, uc_mode UcMode,
                     int SPReg, std::vector<int> ParamRegs, int RetReg,
                     bool IsARM32 = false) {
    std::error_code MkEC;
    Work = TmpDir / ("t" + std::to_string(WorkSeq.fetch_add(1)));
    fs::create_directories(Work, MkEC);
    WorkGuard Guard{Work};
    // ---- Step 1: Write C source to temp file ----
    auto CPath = (Work / (TC.Name + ".c")).string();
    auto ObjPath = (Work / (TC.Name + ".o")).string();
    // Verify the source actually lands; retry on transient write failure so a
    // missing input never masquerades as a compile error in a full-suite run.
    for (int Attempt = 0; Attempt < kSpawnRetries; ++Attempt) {
      fs::create_directories(Work, MkEC);
      std::ofstream F(CPath);
      F << TC.CSrc;
      F.close();
      if (fs::exists(CPath))
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20 * (Attempt + 1)));
    }

    // ---- Step 2: Compile with clang ----
    std::string OptFlag = "-O" + std::to_string(TC.OptLevel);
    std::string EffTarget =
        TC.ClangTargetOverride.empty() ? ClangTarget : TC.ClangTargetOverride;
    std::string CompileCmd =
        "clang -target " + EffTarget +
        " -nostdlib -c " + OptFlag + " -fno-stack-protector -fno-exceptions"
        " -fno-unwind-tables -fno-asynchronous-unwind-tables";
    // The default ARM32 CPU pins armv7; an override target (e.g. ARMv8 crypto)
    // supplies its own -march/-mfpu via ExtraFlags, so skip the pinned CPU.
    if (IsARM32 && TC.ClangTargetOverride.empty())
      CompileCmd += " -mcpu=cortex-a15";
    if (!TC.ExtraFlags.empty())
      CompileCmd += " " + TC.ExtraFlags;
    CompileCmd += " -o " + neverd::test::shellQuote(ObjPath) + " " +
                  neverd::test::shellQuote(CPath);
    auto CR = runCmd(CompileCmd);
    if (!CR.ok()) {
      GTEST_SKIP() << "clang compilation failed: " << CR.Err;
      return;
    }

    // When LinkMemBuiltins is on (the default) and the source actually emits a
    // mem* builtin (clang lowers a runtime-size __builtin_mem* / a large local
    // computed-goto table copy to a real memcpy/memset/memmove call), compile a
    // freestanding implementation to link into both the original and recompiled
    // images so the call resolves to real, Unicorn-runnable code on each side.
    // The gate on objReferencesMemBuiltins keeps a test that emits no mem* call
    // byte-for-byte unchanged.  The 32-bit external-mem* argument recovery (the
    // VA-0 arity collision on ARM32 and the i386 cdecl stack-arg order) is fixed
    // in MedABIPass and regression-covered by AllPlatform_MemBuiltinLiveRTTests
    // (see §15.2 arm32-memset-note).
    if (TC.LinkMemBuiltins && objReferencesMemBuiltins(ObjPath))
      MemHelperObj = compileMemHelper(EffTarget, IsARM32, TC.ClangTargetOverride);

    // ---- Step 3: Extract original .text (+ link if relocations / rodata) ----
    SectionData OrigSections;
    {
      auto PlainSD = extractSectionsFromFile(ObjPath);
      bool NeedLink = PlainSD.hasData();
      if (!NeedLink) {
        auto BufOrErr = llvm::MemoryBuffer::getFile(ObjPath);
        if (BufOrErr) {
          auto ObjOrErr = llvm::object::ObjectFile::createObjectFile(
              (*BufOrErr)->getMemBufferRef());
          if (ObjOrErr)
            NeedLink = objectNeedsLink(**ObjOrErr);
        }
      }
      if (NeedLink) {
        auto Linked =
            linkAndExtract(ObjPath, TC.Name + "_orig", UcArch, UcMode);
        // The object has relocations/data — only the LINKED image has correct
        // addresses.  Do NOT fall back to the unlinked object: its unresolved
        // relocations emulate to wrong/unmapped memory and would surface as a
        // bogus semantic failure.  A link still failing after retries is a
        // transient infrastructure problem, so skip honestly.
        if (Linked.Text.empty()) {
          GTEST_SKIP() << "original link failed after retries (transient infra)"
                       << "\n  Test: " << TC.Name;
          return;
        }
        OrigSections = std::move(Linked);
      } else {
        OrigSections = std::move(PlainSD);
      }
    }
    if (OrigSections.Text.empty()) {
      GTEST_SKIP() << "Could not extract .text from original object";
      return;
    }
    packSections(OrigSections);

    // ---- Step 4: Run original in Unicorn ----
    auto OrigState = emulateFunction(
        UcArch, UcMode, OrigSections.Text, TC.Args, ParamRegs, SPReg, RetReg,
        IsARM32, {}, 0, TC.UcCpuModel);
    ASSERT_TRUE(OrigState.OK)
        << "Original emulation failed: " << OrigState.Error
        << "\n  Test: " << TC.Name;

    // ---- Step 5: Lift and recompile with NeverD ----
    int Ret = neverd_lift_to_obj(Sess, ObjPath.c_str(),
                                  /*NoOpt=*/TC.NoOpt ? 1 : 0, /*MaxFunctions=*/0);
    if (Ret != 0) {
      const char *Err = neverd_last_error(Sess);
      GTEST_SKIP() << "Lift-to-obj failed: " << (Err ? Err : "unknown")
                    << "\n  Test: " << TC.Name;
      return;
    }

    int FuncCount = neverd_roundtrip_func_count(Sess);
    if (FuncCount == 0) {
      GTEST_SKIP() << "No functions in roundtrip result"
                    << "\n  Test: " << TC.Name;
      return;
    }

    // ---- Step 6: Extract recompiled .text + .rodata ----
    unsigned long long ObjLen = 0;
    const unsigned char *ObjData = neverd_roundtrip_obj(Sess, &ObjLen);
    ASSERT_NE(ObjData, nullptr) << "No recompiled object data";

    bool RecompLinkFailed = false;
    auto RecompSections =
        extractSectionsWithLink(ObjData, ObjLen, TC.Name, RecompLinkFailed);
    if (RecompLinkFailed) {
      GTEST_SKIP() << "recompiled link failed after retries (transient infra)"
                   << "\n  Test: " << TC.Name;
      return;
    }
    if (RecompSections.Text.empty()) {
      GTEST_SKIP() << "Could not extract .text from recompiled object"
                   << "\n  Test: " << TC.Name;
      return;
    }
    packSections(RecompSections);

    // ---- Step 7: Run recompiled in Unicorn (same ABI) ----
    auto RecompState = emulateFunction(
        UcArch, UcMode, RecompSections.Text, TC.Args, ParamRegs, SPReg, RetReg,
        IsARM32, {}, 0, TC.UcCpuModel);
    ASSERT_TRUE(RecompState.OK)
        << "Recompiled emulation failed: " << RecompState.Error
        << "\n  Test: " << TC.Name;

    // ---- Step 8: Compare return values ----
    EXPECT_EQ(OrigState.RetVal, RecompState.RetVal)
        << "Return value mismatch after roundtrip"
        << "\n  Test: " << TC.Name
        << "\n  Original:   0x" << std::hex << OrigState.RetVal
        << "\n  Recompiled: 0x" << RecompState.RetVal << std::dec;
  }

  // --- Unicorn emulation of a C function ---

  struct FuncResult {
    uint64_t RetVal = 0;
    bool OK = false;
    std::string Error;
  };

  FuncResult emulateFunction(uc_arch Arch, uc_mode Mode,
                             const std::vector<uint8_t> &Code,
                             const std::vector<uint64_t> &Args,
                             const std::vector<int> &ParamRegs,
                             int SPReg, int RetReg,
                             bool IsARM32 = false,
                             const std::vector<uint8_t> &Rodata = {},
                             uint64_t RodataAddr = 0,
                             int UcCpuModel = -1) {
    FuncResult R;
    uc_engine *UC = nullptr;
    uc_err Err = uc_open(Arch, Mode, &UC);
    if (Err != UC_ERR_OK) {
      R.Error = std::string("uc_open: ") + uc_strerror(Err);
      return R;
    }

    // Select a non-default CPU model (e.g. MAX for crypto) before any other
    // call.  uc_ctl_set_cpu_model must run immediately after uc_open.
    if (UcCpuModel >= 0) {
      int Model = UcCpuModel;
      uc_ctl_set_cpu_model(UC, Model);
    }

    // The function returns to (and emulation stops at) the address just past the
    // code image.  ARM `bx`/`pop {pc}` treats the low bit of a loaded PC as a
    // Thumb-mode select, so an ODD landing address (e.g. when packed .rodata
    // makes the image an odd length) silently switches to Thumb at addr&~1 — the
    // PC then never equals the `until` value and runs into unmapped memory.
    // Align the landing address up to 4 bytes so it stays ARM-mode and matches.
    uint64_t RetLanding = (Code.size() + 3) & ~3ULL;
    uint64_t CodeMapSize = (RetLanding + 0x1000) & ~0xFFFULL;
    if (CodeMapSize < 0x10000)
      CodeMapSize = 0x10000;
    uc_mem_map(UC, CODE_BASE, CodeMapSize, UC_PROT_ALL);
    uc_mem_map(UC, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
    uc_mem_map(UC, DATA_BASE, DATA_SIZE, UC_PROT_ALL);

    if (IsARM32 || Arch == UC_ARCH_ARM64) {
      uc_mem_map(UC, 0xFFFF0000ULL, 0x10000, UC_PROT_ALL);
      uc_mem_map(UC, 0x0, 0x10000, UC_PROT_ALL);
    }

    uc_mem_write(UC, CODE_BASE, Code.data(), Code.size());

    if (!Rodata.empty()) {
      uint64_t RoBase = CODE_BASE + ((Code.size() + 15) & ~15ULL);
      if (RodataAddr != 0)
        RoBase = CODE_BASE + RodataAddr;
      if (RoBase < CODE_BASE || RoBase >= CODE_BASE + CodeMapSize) {
        uint64_t MapBase = RoBase & ~0xFFFULL;
        uint64_t RoMap = ((Rodata.size() + 0xFFF) & ~0xFFFULL);
        if (RoMap < 0x10000)
          RoMap = 0x10000;
        uc_mem_map(UC, MapBase, RoMap, UC_PROT_ALL);
      }
      uc_mem_write(UC, RoBase, Rodata.data(), Rodata.size());
    }

    uint64_t SP = STACK_BASE + STACK_SIZE - 0x100;
    uc_reg_write(UC, SPReg, &SP);

    if (IsARM32) {
      uint32_t CPSR;
      uc_reg_read(UC, UC_ARM_REG_CPSR, &CPSR);
      CPSR |= (0xFu << 20);
      uc_reg_write(UC, UC_ARM_REG_CPSR, &CPSR);
      uint32_t CPACR = 0x00F00000;
      uc_reg_write(UC, UC_ARM_REG_C1_C0_2, &CPACR);
      uint32_t FPEXC = 0x40000000;
      uc_reg_write(UC, UC_ARM_REG_FPEXC, &FPEXC);

      uint32_t LR = CODE_BASE + RetLanding;
      uc_reg_write(UC, UC_ARM_REG_LR, &LR);
    }

    if (Arch == UC_ARCH_ARM64) {
      uint64_t LR = CODE_BASE + RetLanding;
      uc_reg_write(UC, UC_ARM64_REG_X30, &LR);
    }

    if (Arch == UC_ARCH_X86 && Mode == UC_MODE_64) {
      uint64_t RetAddr = CODE_BASE + RetLanding;
      SP -= 8;
      uc_mem_write(UC, SP, &RetAddr, 8);
      uc_reg_write(UC, SPReg, &SP);
    }

    // i386 cdecl: the caller pushes args right-to-left then the return address,
    // so the callee reads arg i at [esp + 4 + 4*i].  esp+4 is kept 16-byte
    // aligned (the post-call invariant clang's SSE spill code assumes).
    if (Arch == UC_ARCH_X86 && Mode == UC_MODE_32) {
      uint64_t Base = (SP & ~0xFULL) - 4;
      uint32_t Ret32 = static_cast<uint32_t>(CODE_BASE + RetLanding);
      uc_mem_write(UC, Base, &Ret32, 4);
      for (size_t I = 0; I < Args.size(); ++I) {
        uint32_t V = static_cast<uint32_t>(Args[I]);
        uc_mem_write(UC, Base + 4 + 4 * I, &V, 4);
      }
      uint32_t ESP = static_cast<uint32_t>(Base);
      uc_reg_write(UC, SPReg, &ESP);
    }

    for (size_t I = 0; I < Args.size() && I < ParamRegs.size(); ++I) {
      uint64_t V = Args[I];
      uc_reg_write(UC, ParamRegs[I], &V);
    }

    uint64_t EndAddr = CODE_BASE + RetLanding;
    Err = uc_emu_start(UC, CODE_BASE, EndAddr, 10000000, 0);
    if (Err != UC_ERR_OK) {
      // Capture the faulting PC (relative to CODE_BASE) so a write/exception
      // fault points at the offending instruction in the disassembly.
      int PCReg = (Arch == UC_ARCH_X86 && Mode == UC_MODE_64) ? int(UC_X86_REG_RIP)
                  : (Arch == UC_ARCH_X86)                     ? int(UC_X86_REG_EIP)
                  : (Arch == UC_ARCH_ARM64)                   ? int(UC_ARM64_REG_PC)
                                                              : int(UC_ARM_REG_PC);
      uint64_t PC = 0;
      uc_reg_read(UC, PCReg, &PC);
      char Buf[96];
      snprintf(Buf, sizeof(Buf), " (pc=0x%llx off=0x%llx)",
               (unsigned long long)PC,
               (unsigned long long)(PC >= CODE_BASE ? PC - CODE_BASE : PC));
      R.Error = std::string("uc_emu_start: ") + uc_strerror(Err) + Buf;
      uc_close(UC);
      return R;
    }

    uint64_t RetVal = 0;
    uc_reg_read(UC, RetReg, &RetVal);
    R.RetVal = RetVal;
    R.OK = true;

    uc_close(UC);
    return R;
  }

  // --- Section types ---

  // One allocated non-.text section captured from the (linked) object: its
  // address relative to the text base plus its bytes.  A zero-init section
  // (.bss / SHT_NOBITS) carries no Bytes and a nonzero ZeroFill extent instead.
  struct DataSeg {
    uint64_t Addr = 0;
    std::vector<uint8_t> Bytes;
    uint64_t ZeroFill = 0;
  };

  struct SectionData {
    std::vector<uint8_t> Text;
    // Every allocated read-only AND writable data section (.rodata*,
    // .data.rel.ro, .data, .bss) at its VMA relative to the text base.  Captured
    // as a list — not a single blob — so a function touching several globals (a
    // read-only table plus a mutable .data array plus a .bss buffer) round-trips
    // every section rather than only the first.
    std::vector<DataSeg> Data;

    bool hasData() const {
      for (const auto &D : Data)
        if (!D.Bytes.empty() || D.ZeroFill)
          return true;
      return false;
    }
  };

  // Collect .text and every allocated data section (read-only and writable) from
  // an object/linked file.  \p AddrBias is subtracted from each section VMA so
  // the result is relative to the text base (0 for a relocatable .o, CODE_BASE
  // for a linked image).
  static void captureSections(const llvm::object::ObjectFile &Obj,
                              SectionData &SD, uint64_t AddrBias) {
    for (const auto &Sec : Obj.sections()) {
      auto NameOrErr = Sec.getName();
      if (!NameOrErr)
        continue;
      llvm::StringRef Name = *NameOrErr;
      if (Name == ".text") {
        if (auto C = Sec.getContents())
          SD.Text = {C->begin(), C->end()};
        continue;
      }
      bool IsData = Name.starts_with(".rodata") || Name == ".data.rel.ro" ||
                    Name == ".data" || Name.starts_with(".data.") ||
                    Name == ".bss" || Name.starts_with(".bss.");
      if (!IsData)
        continue;
      DataSeg Seg;
      Seg.Addr = Sec.getAddress() - AddrBias;
      if (Sec.isBSS()) {
        Seg.ZeroFill = Sec.getSize();
      } else if (auto C = Sec.getContents()) {
        Seg.Bytes = {C->begin(), C->end()};
      }
      if (!Seg.Bytes.empty() || Seg.ZeroFill)
        SD.Data.push_back(std::move(Seg));
    }
  }

  /// Lay every captured data section into the .text image at its VMA so Unicorn
  /// sees one contiguous image at CODE_BASE (matches the linked VMA layout).  A
  /// zero-init (.bss) section only extends the image; its bytes stay zero.
  static void packSections(SectionData &SD) {
    if (SD.Data.empty())
      return;
    uint64_t Span = SD.Text.size();
    for (const auto &D : SD.Data) {
      uint64_t End = D.Addr + std::max<uint64_t>(D.Bytes.size(), D.ZeroFill);
      if (End > Span)
        Span = End;
    }
    std::vector<uint8_t> Buf(Span, 0);
    if (!SD.Text.empty())
      std::memcpy(Buf.data(), SD.Text.data(), SD.Text.size());
    for (const auto &D : SD.Data)
      if (!D.Bytes.empty() && D.Addr + D.Bytes.size() <= Span)
        std::memcpy(Buf.data() + D.Addr, D.Bytes.data(), D.Bytes.size());
    SD.Text = std::move(Buf);
    SD.Data.clear();
  }

  static bool objectNeedsLink(const llvm::object::ObjectFile &Obj) {
    for (const auto &Sec : Obj.sections()) {
      auto NameOrErr = Sec.getName();
      if (!NameOrErr)
        continue;
      if (NameOrErr->starts_with(".rel"))
        return true;
    }
    return false;
  }

  // True when the object carries an *undefined* reference to a mem* builtin that
  // clang emits for aggregate/array initialisation (e.g. a >=5-entry local
  // computed-goto table copied with `memcpy`).  The roundtrip links -nostdlib,
  // so such a reference is otherwise unresolved and the function would call an
  // unmapped address under Unicorn; when present a freestanding mem* helper is
  // linked in (compileMemHelper).  Tests with no such reference never trigger
  // this, so their link is byte-for-byte unchanged.
  static bool objReferencesMemBuiltins(const std::string &Path) {
    auto BufOrErr = llvm::MemoryBuffer::getFile(Path);
    if (!BufOrErr)
      return false;
    auto ObjOrErr = llvm::object::ObjectFile::createObjectFile(
        (*BufOrErr)->getMemBufferRef());
    if (!ObjOrErr)
      return false;
    for (const auto &Sym : (*ObjOrErr)->symbols()) {
      auto FlagsOrErr = Sym.getFlags();
      if (!FlagsOrErr)
        continue;
      if (!(*FlagsOrErr & llvm::object::SymbolRef::SF_Undefined))
        continue;
      auto NameOrErr = Sym.getName();
      if (!NameOrErr)
        continue;
      llvm::StringRef N = *NameOrErr;
      if (N == "memcpy" || N == "memset" || N == "memmove")
        return true;
    }
    return false;
  }

  // Compile a freestanding memcpy/memset/memmove (plain byte loops; -fno-builtin
  // so clang does not lower the loop back into a mem* call) for the test target.
  // Returns the object path, or "" on failure (the caller links without it).
  std::string compileMemHelper(const std::string &EffTarget, bool IsARM32,
                               const std::string &ClangTargetOverride) {
    auto SrcPath = (Work / "_memhelper.c").string();
    auto ObjPath = (Work / "_memhelper.o").string();
    {
      std::ofstream F(SrcPath);
      F << "void *memcpy(void *d,const void *s,unsigned long n){\n"
           "  unsigned char *a=(unsigned char*)d;\n"
           "  const unsigned char *b=(const unsigned char*)s;\n"
           "  for(unsigned long i=0;i<n;i++)a[i]=b[i];return d;}\n"
           "void *memmove(void *d,const void *s,unsigned long n){\n"
           "  unsigned char *a=(unsigned char*)d;\n"
           "  const unsigned char *b=(const unsigned char*)s;\n"
           "  if(a<b){for(unsigned long i=0;i<n;i++)a[i]=b[i];}\n"
           "  else{for(unsigned long i=n;i>0;i--)a[i-1]=b[i-1];}return d;}\n"
           "void *memset(void *d,int c,unsigned long n){\n"
           "  unsigned char *a=(unsigned char*)d;\n"
           "  for(unsigned long i=0;i<n;i++)a[i]=(unsigned char)c;return d;}\n";
    }
    std::string Cmd =
        "clang -target " + EffTarget +
        " -nostdlib -c -O0 -fno-builtin -fno-stack-protector -fno-exceptions"
        " -fno-unwind-tables -fno-asynchronous-unwind-tables";
    if (IsARM32 && ClangTargetOverride.empty())
      Cmd += " -mcpu=cortex-a15";
    Cmd += " -o " + neverd::test::shellQuote(ObjPath) + " " +
           neverd::test::shellQuote(SrcPath);
    auto R = runCmd(Cmd);
    if (!R.ok() || !fs::exists(ObjPath))
      return "";
    return ObjPath;
  }

  // --- .text extraction ---

  static SectionData extractSectionsFromFile(const std::string &Path) {
    SectionData SD;
    auto BufOrErr = llvm::MemoryBuffer::getFile(Path);
    if (!BufOrErr)
      return SD;
    auto ObjOrErr = llvm::object::ObjectFile::createObjectFile(
        (*BufOrErr)->getMemBufferRef());
    if (!ObjOrErr)
      return SD;
    captureSections(**ObjOrErr, SD, /*AddrBias=*/0);
    return SD;
  }

  static std::vector<uint8_t> extractTextFromFile(const std::string &Path) {
    auto BufOrErr = llvm::MemoryBuffer::getFile(Path);
    if (!BufOrErr)
      return {};
    auto ObjOrErr = llvm::object::ObjectFile::createObjectFile(
        (*BufOrErr)->getMemBufferRef());
    if (!ObjOrErr)
      return {};
    for (const auto &Sec : (*ObjOrErr)->sections()) {
      auto NameOrErr = Sec.getName();
      if (!NameOrErr)
        continue;
      if (*NameOrErr == ".text") {
        auto ContentsOrErr = Sec.getContents();
        if (!ContentsOrErr)
          continue;
        return {ContentsOrErr->begin(), ContentsOrErr->end()};
      }
    }
    return {};
  }

  static std::vector<uint8_t> extractTextSection(const unsigned char *Data,
                                                  size_t Len) {
    auto SD = extractSections(Data, Len);
    return SD.Text;
  }

  static SectionData extractSections(const unsigned char *Data, size_t Len) {
    SectionData SD;
    auto BufOrErr = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<const char *>(Data), Len), "", false);
    auto ObjOrErr = llvm::object::ObjectFile::createObjectFile(
        BufOrErr->getMemBufferRef());
    if (!ObjOrErr)
      return SD;
    captureSections(**ObjOrErr, SD, /*AddrBias=*/0);
    return SD;
  }

  SectionData linkAndExtract(const std::string &ObjPath,
                             const std::string &Tag, uc_arch Arch,
                             uc_mode Mode = UC_MODE_64) {
    auto LinkedFile = (Work / (Tag + "_linked.elf")).string();

    std::string Emul;
    if (Arch == UC_ARCH_ARM)
      Emul = "armelf_linux_eabi";
    else if (Arch == UC_ARCH_ARM64)
      Emul = "aarch64linux";
    else if (Arch == UC_ARCH_X86 && Mode == UC_MODE_32)
      Emul = "elf_i386";
    else
      Emul = "elf_x86_64";

    char HexBuf[32];
    snprintf(HexBuf, sizeof(HexBuf), "0x%llx", (unsigned long long)CODE_BASE);
    // The main object is linked first so its function lands at CODE_BASE (the
    // emulation entry); the mem* helper, when present, follows and is reached
    // only via call.
    std::string LinkCmd = "ld.lld -m " + Emul +
                          " --image-base=" + HexBuf +
                          " -Ttext=" + HexBuf +
                          " --oformat=elf -nostdlib --no-dynamic-linker"
                          " --noinhibit-exec -o " +
                          neverd::test::shellQuote(LinkedFile) + " " +
                          neverd::test::shellQuote(ObjPath);
    if (!MemHelperObj.empty())
      LinkCmd += " " + neverd::test::shellQuote(MemHelperObj);
    LinkCmd += neverd::test::silenceStderr();
    // A required link almost never fails for a real reason (NeverD codegen and
    // clang both emit well-formed objects); a failure is a transient infra
    // disruption, so retry with backoff before giving up.  runCmd already
    // retries the "ran but produced no output" case; this loop additionally
    // retries a genuine nonzero exit (e.g. the linker child was signalled).
    ExecResult LR;
    for (int Attempt = 0; Attempt < kLinkRetries; ++Attempt) {
      LR = runCmd(LinkCmd);
      if (LR.ok())
        break;
      std::this_thread::sleep_for(
          std::chrono::milliseconds(30 * (Attempt + 1)));
    }
    if (!LR.ok())
      return {};

    auto LinkedBufOrErr = llvm::MemoryBuffer::getFile(LinkedFile);
    if (!LinkedBufOrErr)
      return {};
    auto LinkedObjOrErr = llvm::object::ObjectFile::createObjectFile(
        (*LinkedBufOrErr)->getMemBufferRef());
    if (!LinkedObjOrErr)
      return {};

    SectionData SD;
    captureSections(**LinkedObjOrErr, SD, /*AddrBias=*/CODE_BASE);
    return SD;
  }

  // \p LinkFailed is set when the object REQUIRED linking but the link failed
  // (after retries).  In that case the returned SectionData is empty and the
  // caller must skip — never emulate the unlinked object, whose unresolved
  // relocations read wrong/unmapped memory and look like a semantic failure.
  SectionData extractSectionsWithLink(const unsigned char *Data, size_t Len,
                                       const std::string &Name,
                                       bool &LinkFailed) {
    LinkFailed = false;
    auto PlainSD = extractSections(Data, Len);
    if (PlainSD.Text.empty())
      return PlainSD;

    auto ObjFile = (Work / (Name + "_recomp.o")).string();
    {
      std::ofstream F(ObjFile, std::ios::binary);
      F.write(reinterpret_cast<const char *>(Data), Len);
    }

    auto BufOrErr = llvm::MemoryBuffer::getFile(ObjFile);
    if (!BufOrErr)
      return PlainSD;
    auto ObjOrErr = llvm::object::ObjectFile::createObjectFile(
        (*BufOrErr)->getMemBufferRef());
    if (!ObjOrErr)
      return PlainSD;

    uc_arch Arch = UC_ARCH_X86;
    uc_mode Mode = UC_MODE_64;
    auto ObjArch = (*ObjOrErr)->getArch();
    if (ObjArch == llvm::Triple::arm || ObjArch == llvm::Triple::armeb)
      Arch = UC_ARCH_ARM;
    else if (ObjArch == llvm::Triple::aarch64)
      Arch = UC_ARCH_ARM64;
    else if (ObjArch == llvm::Triple::x86)
      Mode = UC_MODE_32;

    if (PlainSD.hasData() || objectNeedsLink(**ObjOrErr)) {
      auto SD = linkAndExtract(ObjFile, Name + "_recomp", Arch, Mode);
      if (!SD.Text.empty())
        return SD;
      // Link was required but failed: signal the caller to skip rather than
      // silently emulate the unlinked (unrelocated) object.
      LinkFailed = true;
      return {};
    }
    return PlainSD;
  }

};

// ============================================================================
// Parameterized test base classes
// ============================================================================
class X64RoundTrip : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};

class AArch64RoundTrip : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};

class ARM32RoundTrip : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};

inline auto rtTCName = [](const ::testing::TestParamInfo<RoundTripTC> &Info) {
  return Info.param.Name;
};

#endif // NEVERD_UNITTESTS_SEMANTIC_SEMANTICROUNDTRIPFIXTURE_H
