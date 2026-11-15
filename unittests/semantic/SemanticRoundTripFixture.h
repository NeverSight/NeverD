//===- SemanticRoundTripFixture.h - Lift roundtrip test fixture --*- C++
//-*-===//
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
#include "FailureSnapshotInput.h"
#include "UnicornSemanticFixture.h"

#include "neverd/libc/LibCNames.h"
#include "neverd/object/SectionNames.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

// ============================================================================
// Roundtrip test-case descriptor
// ============================================================================
enum class RecoveredSwitchExpectation : uint8_t {
  Unspecified,
  Required,
  Forbidden,
};

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
  /// recompiled images so a mem* builtin the source emits (e.g. clang's
  /// `memcpy` for a >=5-entry local computed-goto table, or any stack-array
  /// memset) resolves to real, Unicorn-runnable code instead of the unresolved
  /// -nostdlib placeholder.  Default ON: the helper links into BOTH the
  /// original and the recompiled image, so a test that copies/zeroes a buffer
  /// genuinely executes it on both sides instead of comparing two un-cleared
  /// stack-garbage runs that only happen to match (an "accidental pass").  The
  /// harness gates the actual link on objReferencesMemBuiltins, so a test that
  /// emits no mem* call is unaffected. (External-mem* argument recovery — the
  /// VA-0 arity collision and the i386 cdecl stack-arg order — is fixed in
  /// MedABIPass; see §15.2 arm32-memset-note.)
  bool LinkMemBuiltins = true;

  /// Initial FPCR value for AArch64 emulation.  This lets a test exercise an
  /// instruction under directed rounding without depending on FPCR system-
  /// register lifting, which is a separate semantic concern.
  uint64_t InitialAArch64FPCR = 0;

  /// Structural expectation for the exact target function.  A required
  /// computed-goto switch prevents an indirect-call accidental pass; a
  /// forbidden switch is a fail-closed certificate test and returns after the
  /// IR assertion rather than executing the deliberately unsafe branch shape.
  RecoveredSwitchExpectation RecoveredSwitch =
      RecoveredSwitchExpectation::Unspecified;

  /// Minimum number of switch instructions required in the exact target
  /// function when RecoveredSwitch is Required.  Multi-consumer regressions
  /// use this to prevent one recovered dispatch from hiding a lost sibling.
  uint32_t MinimumRecoveredSwitchCount = 1;

  /// Check the decompiled C by execution in place of the lifted object, on
  /// this route (true: LLVM C, false: HighC), whatever NEVERD_SEMANTIC_HIGHC
  /// and NEVERD_SEMANTIC_LLVMC select. Empty leaves the choice to them.
  std::optional<bool> SourceRoute;
};

/// True when LLVM emission refused a guessed table/code address instead of
/// emitting a stale GEP or an unauthenticated switch.
inline bool isFailClosedJumpTableDiagnostic(const std::string &Text) {
  return Text.find("refusing stale-address fallback") != std::string::npos ||
         Text.find("unproved runtime offset") != std::string::npos ||
         Text.find("ambiguous reachable read-only table-base") !=
             std::string::npos ||
         Text.find("no unique lifted function entry") != std::string::npos ||
         Text.find("relocatable code-address") != std::string::npos;
}

/// Mark named cases as Forbidden switch certificates (x64/i386 only).
inline std::vector<RoundTripTC>
withForbiddenSwitch(std::vector<RoundTripTC> Cases,
                    std::initializer_list<const char *> NameNeedles) {
  for (auto &TC : Cases)
    for (const char *Needle : NameNeedles)
      if (TC.Name.find(Needle) != std::string::npos) {
        TC.RecoveredSwitch = RecoveredSwitchExpectation::Forbidden;
        break;
      }
  return Cases;
}

inline std::ostream &operator<<(std::ostream &OS, const RoundTripTC &TC) {
  return OS << TC.Category << "/" << TC.Name;
}

// ============================================================================
// Main roundtrip fixture
// ============================================================================
class SemanticRoundTripFixture : public ::testing::Test {
protected:
  static size_t functionBodySwitchCount(std::string_view IR,
                                        std::string_view FunctionName) {
    const std::string DefinitionName = "@" + std::string(FunctionName) + "(";
    size_t SearchFrom = 0;
    while (true) {
      const size_t NameAt = IR.find(DefinitionName, SearchFrom);
      if (NameAt == std::string_view::npos)
        break;
      const size_t LineBegin = NameAt == 0 ? 0 : IR.rfind('\n', NameAt - 1) + 1;
      const size_t LineEnd = IR.find('\n', NameAt);
      const std::string_view DefinitionLine = IR.substr(
          LineBegin, LineEnd == std::string_view::npos ? IR.size() - LineBegin
                                                       : LineEnd - LineBegin);
      if (DefinitionLine.find("define ") != std::string_view::npos) {
        const size_t BodyBegin = IR.find('{', NameAt + DefinitionName.size());
        if (BodyBegin == std::string_view::npos)
          return 0;
        const size_t BodyEnd = IR.find("\n}", BodyBegin + 1);
        if (BodyEnd == std::string_view::npos)
          return 0;
        size_t Count = 0;
        size_t SwitchAt = BodyBegin + 1;
        while ((SwitchAt = IR.find("switch i", SwitchAt)) !=
                   std::string_view::npos &&
               SwitchAt < BodyEnd) {
          // A dispatch by value compares the jump's target with the labels
          // it may reach and recovers no table: the emitter marks its switch
          // after the case list, and it does not count.
          const size_t Close = IR.find(']', SwitchAt);
          const size_t CloseEnd = Close == std::string_view::npos
                                      ? std::string_view::npos
                                      : IR.find('\n', Close);
          const bool ByValue =
              Close != std::string_view::npos && Close < BodyEnd &&
              IR.substr(Close, CloseEnd == std::string_view::npos
                                   ? std::string_view::npos
                                   : CloseEnd - Close)
                      .find("!neverd.value-dispatch") != std::string_view::npos;
          if (!ByValue)
            ++Count;
          SwitchAt += std::string_view("switch i").size();
        }
        return Count;
      }
      SearchFrom = NameAt + DefinitionName.size();
    }
    return 0;
  }

  static void SetUpTestSuite() {
    LLVMMCAssembler::initTargets();
    if (!Sess)
      Sess = neverd_session_create();

    // One root per process; every test gets its own unique subdir under it so
    // suites never share files. Suite teardown must NOT wipe this root (other
    // suites follow it in the same process), only per-test guards clean up.
    if (TmpDir.empty())
      TmpDir =
          fs::temp_directory_path() /
          ("neverd_rt_" + std::to_string(neverd::test::currentProcessId()));
    std::error_code EC;
    fs::create_directories(TmpDir, EC);
  }

  static void TearDownTestSuite() {
    if (Sess) {
      neverd_session_destroy(Sess);
      Sess = nullptr;
    }
  }

  // Fixture integrity tests override only the SDK boundary, after successful
  // source compilation and original emulation, to verify failure reporting.
  virtual int liftToObject(neverd_session_t Session, const char *ObjectPath,
                           int NoOpt) {
    return neverd_lift_to_obj(Session, ObjectPath, NoOpt, /*MaxFunctions=*/0);
  }

  virtual int roundTripFunctionCount(neverd_session_t Session) {
    return neverd_roundtrip_func_count(Session);
  }

  // --- Architecture-specific roundtrip runners ---

  void roundTripX64(const RoundTripTC &TC) {
    roundTripImpl(TC, "x86_64-linux-gnu", "x86_64-linux-gnu", UC_ARCH_X86,
                  UC_MODE_64, UC_X86_REG_RSP,
                  {UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RDX,
                   UC_X86_REG_RCX, UC_X86_REG_R8, UC_X86_REG_R9},
                  UC_X86_REG_RAX);
  }

  // i386 SysV cdecl: arguments arrive on the stack (no parameter registers) and
  // the 32-bit result returns in EAX.  emulateFunction lays out the cdecl frame
  // when it sees UC_ARCH_X86 + UC_MODE_32.
  void roundTripX86(const RoundTripTC &TC) {
    roundTripImpl(TC, "i386-linux-gnu", "i386-linux-gnu", UC_ARCH_X86,
                  UC_MODE_32, UC_X86_REG_ESP, {}, UC_X86_REG_EAX);
  }

  void roundTripAArch64(const RoundTripTC &TC) {
    roundTripImpl(TC, "aarch64-linux-gnu", "aarch64-linux-gnu", UC_ARCH_ARM64,
                  static_cast<uc_mode>(0), UC_ARM64_REG_SP,
                  {UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                   UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5},
                  UC_ARM64_REG_X0);
  }

  void roundTripARM32(const RoundTripTC &TC) {
    roundTripImpl(TC, "armv7-linux-gnueabi", "armv7-linux-gnueabi", UC_ARCH_ARM,
                  UC_MODE_ARM, UC_ARM_REG_SP,
                  {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3},
                  UC_ARM_REG_R0, true);
  }

private:
  // Number of times a temp-file action is retried when it fails with no output
  // at all — the signature of a transient fork/redirect failure under the heavy
  // process churn of a full suite run (not a real tool diagnostic).
  static constexpr int kSpawnRetries = 5;

  // Bound retries of a REQUIRED link (ld.lld), including temporary process
  // disruption. Exhausting retries is a failed execution, regardless of its
  // cause. Never use the unlinked object as a fallback: unresolved relocations
  // would masquerade as a semantic failure during emulation.
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

  /// Path to the ARM EABI floating-to-64-bit conversion routines compiled for
  /// this test's target (compileConversionHelper), or "" when no image calls
  /// one.  Linked into both images as the mem* helper is.
  std::string ConversionHelperObj;

  /// The function the link places first in the text, or "" for the inputs'
  /// own order.
  std::string LinkFirst;

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
      int RC = neverd::test::runShellCommand(
          Cmd + neverd::test::redirectOutput(OutF, ErrF));
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
      std::this_thread::sleep_for(
          std::chrono::milliseconds(20 * (Attempt + 1)));
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
      std::this_thread::sleep_for(
          std::chrono::milliseconds(20 * (Attempt + 1)));
    }

    // ---- Step 2: Compile with clang ----
    std::string OptFlag = "-O" + std::to_string(TC.OptLevel);
    std::string EffTarget =
        TC.ClangTargetOverride.empty() ? ClangTarget : TC.ClangTargetOverride;
    std::string CompileCmd =
        "clang -target " + EffTarget + " -nostdlib -nostdlibinc -c " + OptFlag +
        " -fno-stack-protector -fno-exceptions"
        " -fno-unwind-tables -fno-asynchronous-unwind-tables";
    // The target options the decompiled C builds with too.
    std::string ArchFlags;
    // Use Clang's target intrinsic headers without falling through to the
    // host libc. Keep hosted builtin optimization semantics unchanged.
    // Normalize cross-target defaults across Clang distributions.  Apple Clang
    // enables Cortex-A15 FP/NEON instructions for gnueabi and selects Pentium 4
    // (SSE2) for i386, while Ubuntu Clang defaults those targets to soft-float
    // and i686/x87 respectively.  The semantic grid tests NeverD, so keep its
    // generated input stable instead of testing host-driver policy.  Explicit
    // per-case flags follow these defaults and can still override them.
    if (IsARM32 && TC.ClangTargetOverride.empty())
      ArchFlags += " -mcpu=cortex-a15 -mfloat-abi=softfp";
    else if (UcArch == UC_ARCH_X86 && UcMode == UC_MODE_32 &&
             TC.ClangTargetOverride.empty())
      ArchFlags += " -march=pentium4";
    if (!TC.ExtraFlags.empty())
      ArchFlags += " " + TC.ExtraFlags;
    CompileCmd += ArchFlags + " -o " + neverd::test::shellQuote(ObjPath) + " " +
                  neverd::test::shellQuote(CPath);
    auto CR = runCmd(CompileCmd);
    if (!CR.ok()) {
      GTEST_SKIP() << "clang compilation failed: " << CR.Err;
      return;
    }
    neverd::test::retainFailureSnapshotInput(TC.Name, CPath, ObjPath,
                                             CompileCmd);

    // When LinkMemBuiltins is on (the default) and the source actually emits a
    // mem* builtin (clang lowers a runtime-size __builtin_mem* / a large local
    // computed-goto table copy to a real memcpy/memset/memmove call), compile a
    // freestanding implementation to link into both the original and recompiled
    // images so the call resolves to real, Unicorn-runnable code on each side.
    // The gate on objReferencesMemBuiltins keeps a test that emits no mem* call
    // byte-for-byte unchanged.  The 32-bit external-mem* argument recovery (the
    // VA-0 arity collision on ARM32 and the i386 cdecl stack-arg order) is
    // fixed in MedABIPass and regression-covered by
    // AllPlatform_MemBuiltinLiveRTTests (see §15.2 arm32-memset-note).
    if (TC.LinkMemBuiltins && objReferencesMemBuiltins(ObjPath))
      MemHelperObj =
          compileMemHelper(EffTarget, IsARM32, TC.ClangTargetOverride);
    // A conversion of a floating value to a 64-bit integer is a call to the
    // compiler's run-time routine on AArch32.  Unlinked, the call does
    // nothing and each image returns what its registers happened to hold.
    if (objReferences(ObjPath, isLinkedConversionName))
      ConversionHelperObj =
          compileConversionHelper(EffTarget, IsARM32, TC.ClangTargetOverride);

    // ---- Step 3: Extract original .text (+ link if relocations / rodata) ----
    SectionData OrigSections;
    {
      auto PlainSD = extractSectionsFromFile(ObjPath);
      bool NeedLink = PlainSD.requiresLink();
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
        // The object has relocations/data or split text — only the LINKED image
        // has correct addresses and section layout.  Do NOT fall back to the
        // unlinked object: unresolved relocations emulate to wrong/unmapped
        // memory and would surface as a bogus semantic failure.
        if (Linked.State == LinkState::Unavailable)
          GTEST_SKIP() << "original link unavailable: " << Linked.diagnostic()
                       << "\n  Test: " << TC.Name;
        ASSERT_EQ(Linked.State, LinkState::Ready)
            << "original link failed: " << Linked.diagnostic()
            << "\n  Test: " << TC.Name;
        OrigSections = std::move(Linked.Sections);
      } else {
        OrigSections = std::move(PlainSD);
      }
    }
    ASSERT_FALSE(OrigSections.Text.empty())
        << "Could not extract executable text from original object"
        << "\n  Test: " << TC.Name;
    packSections(OrigSections);

    // ---- Step 4: Run original in Unicorn ----
    auto OrigState = emulateFunction(UcArch, UcMode, OrigSections.Text, TC.Args,
                                     ParamRegs, SPReg, RetReg, IsARM32, {}, 0,
                                     TC.UcCpuModel, TC.InitialAArch64FPCR);
    ASSERT_TRUE(OrigState.OK)
        << "Original emulation failed: " << OrigState.Error
        << "\n  Test: " << TC.Name;

    // ---- Step 5 (NEVERD_SEMANTIC_HIGHC / _LLVMC): decompile to C, build it
    // The same comparison then checks the decompiled C by execution: the
    // module is compiled for the original target, linked and run.
    if (TC.SourceRoute) {
      sourceRoundTrip(TC, ObjPath, EffTarget, ArchFlags, UcArch, UcMode, SPReg,
                      ParamRegs, RetReg, IsARM32, OrigState.RetVal,
                      *TC.SourceRoute);
      return;
    }
    for (const auto &[Variable, LlvmRoute] :
         {std::pair{"NEVERD_SEMANTIC_HIGHC", false},
          std::pair{"NEVERD_SEMANTIC_LLVMC", true}})
      if (const char *Value = std::getenv(Variable); Value && *Value) {
        sourceRoundTrip(TC, ObjPath, EffTarget, ArchFlags, UcArch, UcMode,
                        SPReg, ParamRegs, RetReg, IsARM32, OrigState.RetVal,
                        LlvmRoute);
        return;
      }

    // ---- Step 5: Lift and recompile with NeverD ----
    const bool ExpectFailClosedSwitch =
        TC.RecoveredSwitch == RecoveredSwitchExpectation::Forbidden;
    if (ExpectFailClosedSwitch)
      testing::internal::CaptureStderr();
    int Ret = liftToObject(Sess, ObjPath.c_str(), /*NoOpt=*/TC.NoOpt ? 1 : 0);
    const std::string ClosedStderr =
        ExpectFailClosedSwitch ? testing::internal::GetCapturedStderr()
                               : std::string();
    if (Ret != 0) {
      const char *Err = neverd_last_error(Sess);
      const std::string Diagnostic = Err ? Err : "unknown";
      neverd_free_string(Err);
      if (ExpectFailClosedSwitch &&
          isFailClosedJumpTableDiagnostic(ClosedStderr + Diagnostic))
        return;
      FAIL() << "Lift-to-obj failed: " << Diagnostic << "\n  Status: " << Ret
             << "\n  Test: " << TC.Name << "\n  Object: " << ObjPath
             << (ClosedStderr.empty() ? "" : "\n  Stderr: " + ClosedStderr);
    }

    if (TC.RecoveredSwitch != RecoveredSwitchExpectation::Unspecified) {
      const char *RawIR = neverd_roundtrip_ir(Sess);
      ASSERT_NE(RawIR, nullptr) << "No roundtrip IR for " << TC.Name;
      const std::string RoundTripIR(RawIR);
      neverd_free_string(RawIR);
      const size_t SwitchCount = functionBodySwitchCount(RoundTripIR, TC.Name);
      if (TC.RecoveredSwitch == RecoveredSwitchExpectation::Required) {
        ASSERT_GT(TC.MinimumRecoveredSwitchCount, 0u)
            << "required switch count must be positive"
            << "\n  Test: " << TC.Name;
        ASSERT_GE(SwitchCount,
                  static_cast<size_t>(TC.MinimumRecoveredSwitchCount))
            << "not every required computed-goto dispatch was recovered as a "
               "switch in the target function"
            << "\n  Test: " << TC.Name;
      } else {
        ASSERT_EQ(SwitchCount, 0u)
            << "unauthenticated computed-goto frame storage was recovered as "
               "a switch in the target function"
            << "\n  Test: " << TC.Name;
        return;
      }
    }

    int FuncCount = roundTripFunctionCount(Sess);
    ASSERT_GT(FuncCount, 0)
        << "No functions in roundtrip result"
        << "\n  Test: " << TC.Name << "\n  Object: " << ObjPath;

    // ---- Step 6: Extract recompiled .text + .rodata ----
    unsigned long long ObjLen = 0;
    const unsigned char *ObjData = neverd_roundtrip_obj(Sess, &ObjLen);
    ASSERT_NE(ObjData, nullptr) << "No recompiled object data";

    auto Recompiled = extractSectionsWithLink(ObjData, ObjLen, TC.Name);
    if (Recompiled.State == LinkState::Unavailable)
      GTEST_SKIP() << "recompiled link unavailable: " << Recompiled.diagnostic()
                   << "\n  Test: " << TC.Name;
    ASSERT_EQ(Recompiled.State, LinkState::Ready)
        << "recompiled object preparation failed: " << Recompiled.diagnostic()
        << "\n  Test: " << TC.Name;
    auto RecompSections = std::move(Recompiled.Sections);
    ASSERT_FALSE(RecompSections.Text.empty())
        << "Could not extract executable text from recompiled object"
        << "\n  Test: " << TC.Name;
    packSections(RecompSections);

    // ---- Step 7: Run recompiled in Unicorn (same ABI) ----
    auto RecompState = emulateFunction(
        UcArch, UcMode, RecompSections.Text, TC.Args, ParamRegs, SPReg, RetReg,
        IsARM32, {}, 0, TC.UcCpuModel, TC.InitialAArch64FPCR);
    ASSERT_TRUE(RecompState.OK)
        << "Recompiled emulation failed: " << RecompState.Error
        << "\n  Test: " << TC.Name;

    // ---- Step 8: Compare return values ----
    EXPECT_EQ(OrigState.RetVal, RecompState.RetVal)
        << "Return value mismatch after roundtrip"
        << "\n  Test: " << TC.Name << "\n  Original:   0x" << std::hex
        << OrigState.RetVal << "\n  Recompiled: 0x" << RecompState.RetVal
        << std::dec;
  }

  /// Decompiles \p ObjPath to C with the HighC route, or the LLVM-C route
  /// when \p LlvmRoute, compiles it for the original target, and compares its
  /// result with \p Expected.
  void sourceRoundTrip(const RoundTripTC &TC, const std::string &ObjPath,
                       const std::string &EffTarget,
                       const std::string &ArchFlags, uc_arch UcArch,
                       uc_mode UcMode, int SPReg,
                       const std::vector<int> &ParamRegs, int RetReg,
                       bool IsARM32, uint64_t Expected, bool LlvmRoute) {
    const std::string Route = LlvmRoute ? "LLVM-C" : "HighC";
    const std::string Tag = LlvmRoute ? "_llvmc" : "_highc";
    const char *Raw = neverd_decompile_all(Sess, ObjPath.c_str(),
                                           /*UseLlvmRoute=*/LlvmRoute ? 1 : 0,
                                           TC.NoOpt ? 1 : 0,
                                           /*MaxFunctions=*/0);
    std::string Source = Raw ? Raw : "";
    neverd_free_string(Raw);
    if (Source.empty()) {
      const char *Err = neverd_last_error(Sess);
      const std::string Diagnostic = Err ? Err : "unknown";
      neverd_free_string(Err);
      FAIL() << Route << " decompile failed: " << Diagnostic
             << "\n  Test: " << TC.Name;
    }
    const auto CPath = (Work / (TC.Name + Tag + ".c")).string();
    const auto SourceObj = (Work / (TC.Name + Tag + ".o")).string();
    {
      std::ofstream F(CPath);
      F << Source;
      F.close();
      ASSERT_TRUE(static_cast<bool>(F)) << "could not write " << CPath;
    }
    // The output's build contract: plain pointer casts at any address.
    const std::string CompileCmd =
        "clang -target " + EffTarget + " -nostdlib -c -O2 -w" +
        " -ffunction-sections" +
        " -fno-strict-aliasing -fno-stack-protector -fno-exceptions"
        " -fno-unwind-tables -fno-asynchronous-unwind-tables" +
        ArchFlags + " -o " + neverd::test::shellQuote(SourceObj) + " " +
        neverd::test::shellQuote(CPath);
    const auto CR = runCmd(CompileCmd);
    ASSERT_TRUE(CR.ok()) << Route << " compilation failed: " << CR.Err
                         << "\n  Test: " << TC.Name << "\n"
                         << Source;
    // A library routine the C calls (a rounding instruction printed as
    // nearbyint) has no code to link in this freestanding image, and
    // emulation starts at the first code of the section.
    const ObjectLinkage Linkage = objectLinkage(SourceObj);
    if (!Linkage.Externals.empty())
      GTEST_SKIP() << Route << " output calls " << Linkage.Externals.front()
                   << ", which the test does not link\n  Test: " << TC.Name;
    // Each function has its own section; the link puts the tested one
    // first, where emulation starts.
    const std::string Entry = objectLinkage(ObjPath).Entry;
    if (Entry.empty() || !Linkage.Functions.count(Entry))
      GTEST_SKIP() << Route << " output does not define " << Entry
                   << "\n  Test: " << TC.Name;
    LinkFirst = Entry;
    if (MemHelperObj.empty() && objReferencesMemBuiltins(SourceObj))
      MemHelperObj =
          compileMemHelper(EffTarget, IsARM32, TC.ClangTargetOverride);
    if (ConversionHelperObj.empty() &&
        objReferences(SourceObj, isLinkedConversionName))
      ConversionHelperObj =
          compileConversionHelper(EffTarget, IsARM32, TC.ClangTargetOverride);
    auto Buffer = llvm::MemoryBuffer::getFile(SourceObj);
    ASSERT_TRUE(static_cast<bool>(Buffer)) << "could not read " << SourceObj;
    auto Recompiled = extractSectionsWithLink(
        reinterpret_cast<const unsigned char *>((*Buffer)->getBufferStart()),
        (*Buffer)->getBufferSize(), TC.Name + Tag);
    if (Recompiled.State == LinkState::Unavailable)
      GTEST_SKIP() << Route << " link unavailable: " << Recompiled.diagnostic()
                   << "\n  Test: " << TC.Name;
    ASSERT_EQ(Recompiled.State, LinkState::Ready)
        << Route << " object preparation failed: " << Recompiled.diagnostic()
        << "\n  Test: " << TC.Name << "\n"
        << Source;
    auto Sections = std::move(Recompiled.Sections);
    packSections(Sections);
    auto State = emulateFunction(UcArch, UcMode, Sections.Text, TC.Args,
                                 ParamRegs, SPReg, RetReg, IsARM32, {}, 0,
                                 TC.UcCpuModel, TC.InitialAArch64FPCR);
    ASSERT_TRUE(State.OK) << Route << " emulation failed: " << State.Error
                          << "\n  Test: " << TC.Name << "\n"
                          << Source;
    EXPECT_EQ(Expected, State.RetVal)
        << "Return value mismatch after " << Route << " roundtrip"
        << "\n  Test: " << TC.Name << "\n  Original: 0x" << std::hex << Expected
        << "\n  " << Route << ": 0x" << State.RetVal << std::dec << "\n"
        << Source;
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
                             const std::vector<int> &ParamRegs, int SPReg,
                             int RetReg, bool IsARM32 = false,
                             const std::vector<uint8_t> &Rodata = {},
                             uint64_t RodataAddr = 0, int UcCpuModel = -1,
                             uint64_t InitialAArch64FPCR = 0) {
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
    // A process starts with the x87 control word the System V psABI gives
    // it, 0x037F: every exception masked, round to nearest, 64-bit
    // precision.  Unicorn resets the word to zero, which code that saves it
    // with fnstcw and restores it with fldcw turns into 24-bit precision.
    if (Arch == UC_ARCH_X86) {
      uint64_t ControlWord = 0x037F;
      uc_reg_write(UC, UC_X86_REG_FPCW, &ControlWord);
    }

    // The function returns to (and emulation stops at) the address just past
    // the code image.  ARM `bx`/`pop {pc}` treats the low bit of a loaded PC as
    // a Thumb-mode select, so an ODD landing address (e.g. when packed .rodata
    // makes the image an odd length) silently switches to Thumb at addr&~1 —
    // the PC then never equals the `until` value and runs into unmapped memory.
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
      uint64_t FPCR = InitialAArch64FPCR;
      uc_reg_write(UC, UC_ARM64_REG_FPCR, &FPCR);
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
      int PCReg = (Arch == UC_ARCH_X86 && Mode == UC_MODE_64)
                      ? int(UC_X86_REG_RIP)
                  : (Arch == UC_ARCH_X86)   ? int(UC_X86_REG_EIP)
                  : (Arch == UC_ARCH_ARM64) ? int(UC_ARM64_REG_PC)
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
    // A relocatable input may place all code in a split section such as
    // `.text.unlikely.` while leaving the canonical `.text` section empty.
    // Do not concatenate such sections at VMA zero: their relocations and
    // relative layout become valid only after linking.  This bit records that
    // executable input exists and forces the common link-and-extract path.
    bool HasSplitText = false;
    // Every allocated read-only AND writable data section (.rodata*,
    // .data.rel.ro, .data, .bss) at its VMA relative to the text base. Captured
    // as a list — not a single blob — so a function touching several globals (a
    // read-only table plus a mutable .data array plus a .bss buffer)
    // round-trips every section rather than only the first.
    std::vector<DataSeg> Data;

    bool hasData() const {
      for (const auto &D : Data)
        if (!D.Bytes.empty() || D.ZeroFill)
          return true;
      return false;
    }

    bool hasText() const { return !Text.empty() || HasSplitText; }
    bool requiresLink() const { return HasSplitText || hasData(); }
  };

  enum class LinkState { Ready, Unavailable, Failed };

  // Own execution evidence alongside the sections it produced. Execution is
  // absent when linking was unnecessary or no linker could be found; this is
  // distinct from a tool that ran and returned a nonzero status.
  struct LinkOutcome {
    LinkState State = LinkState::Failed;
    SectionData Sections;
    std::optional<ExecResult> Execution;
    std::string Command;
    std::string Error;

    std::string diagnostic() const {
      std::ostringstream OS;
      OS << Error;
      if (!Command.empty())
        OS << "\n  Command: " << Command;
      if (Execution) {
        OS << "\n  Exit status: " << Execution->Code;
        if (!Execution->Out.empty())
          OS << "\n  stdout: " << Execution->Out;
        if (!Execution->Err.empty())
          OS << "\n  stderr: " << Execution->Err;
      }
      return OS.str();
    }
  };

  // Collect .text and every allocated data section (read-only and writable)
  // from an object/linked file.  \p AddrBias is subtracted from each section
  // VMA so the result is relative to the text base (0 for a relocatable .o,
  // CODE_BASE for a linked image).
  static void captureSections(const llvm::object::ObjectFile &Obj,
                              SectionData &SD, uint64_t AddrBias) {
    for (const auto &Sec : Obj.sections()) {
      auto NameOrErr = Sec.getName();
      if (!NameOrErr)
        continue;
      llvm::StringRef Name = *NameOrErr;
      if (neverd::section_names::isTextSectionName(Name)) {
        auto C = Sec.getContents();
        if (!C || C->empty())
          continue;
        if (Name == neverd::section_names::elf::Text)
          SD.Text = {C->begin(), C->end()};
        else
          SD.HasSplitText = true;
        continue;
      }
      bool IsData = neverd::section_names::isElfImageDataSectionName(Name);
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

  // True when the object carries an *undefined* reference to a mem* builtin
  // that clang emits for aggregate/array initialisation (e.g. a >=5-entry local
  // computed-goto table copied with `memcpy`).  The roundtrip links -nostdlib,
  // so such a reference is otherwise unresolved and the function would call an
  // unmapped address under Unicorn; when present a freestanding mem* helper is
  // linked in (compileMemHelper).  Tests with no such reference never trigger
  // this, so their link is byte-for-byte unchanged.
  static bool objReferencesMemBuiltins(const std::string &Path) {
    return objReferences(Path, [](llvm::StringRef N) {
      return neverd::libc::isMemCopyName(N) || neverd::libc::isMemSetName(N);
    });
  }

  /// Whether the object at \p Path leaves a symbol \p Wanted accepts
  /// undefined.
  static bool objReferences(const std::string &Path,
                            bool (*Wanted)(llvm::StringRef)) {
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
      if (Wanted(*NameOrErr))
        return true;
    }
    return false;
  }

  /// The ARM EABI routines that convert a float or a double to a 64-bit
  /// integer, which the test can link (compileConversionHelper).
  static bool isLinkedConversionName(llvm::StringRef N) {
    return N == "__aeabi_d2lz" || N == "__aeabi_d2ulz" || N == "__aeabi_f2lz" ||
           N == "__aeabi_f2ulz";
  }

  /// The functions \p Path calls without defining them, besides the mem* and
  /// conversion routines the test can link, and the function at the start of
  /// its first executable section, where emulation starts.
  struct ObjectLinkage {
    std::vector<std::string> Externals;
    std::string Entry;
    std::set<std::string> Functions;
  };
  static ObjectLinkage objectLinkage(const std::string &Path) {
    ObjectLinkage Result;
    auto BufOrErr = llvm::MemoryBuffer::getFile(Path);
    if (!BufOrErr)
      return Result;
    auto ObjOrErr = llvm::object::ObjectFile::createObjectFile(
        (*BufOrErr)->getMemBufferRef());
    if (!ObjOrErr)
      return Result;
    std::optional<llvm::object::SectionRef> FirstText;
    for (const auto &Section : (*ObjOrErr)->sections())
      if (Section.isText() && Section.getSize()) {
        FirstText = Section;
        break;
      }
    for (const auto &Sym : (*ObjOrErr)->symbols()) {
      auto FlagsOrErr = Sym.getFlags();
      auto NameOrErr = Sym.getName();
      if (!FlagsOrErr || !NameOrErr || NameOrErr->empty())
        continue;
      const llvm::StringRef N = *NameOrErr;
      if (*FlagsOrErr & llvm::object::SymbolRef::SF_Undefined) {
        // The linker defines the GOT position-independent i386 code names.
        if (!neverd::libc::isMemCopyName(N) && !neverd::libc::isMemSetName(N) &&
            !isLinkedConversionName(N) && N != "_GLOBAL_OFFSET_TABLE_")
          Result.Externals.push_back(N.str());
        continue;
      }
      auto Type = Sym.getType();
      auto Value = Sym.getValue();
      auto Section = Sym.getSection();
      if (Type && *Type == llvm::object::SymbolRef::ST_Function)
        Result.Functions.insert(N.str());
      if (FirstText && Type && *Type == llvm::object::SymbolRef::ST_Function &&
          Value && *Value == 0 && Section &&
          *Section != (*ObjOrErr)->section_end() && **Section == *FirstText)
        Result.Entry = N.str();
    }
    return Result;
  }

  // Compile a freestanding memcpy/memset/memmove (plain byte loops;
  // -fno-builtin so clang does not lower the loop back into a mem* call) for
  // the test target. Returns the object path, or "" on failure (the caller
  // links without it).
  std::string compileMemHelper(const std::string &EffTarget, bool IsARM32,
                               const std::string &ClangTargetOverride) {
    return compileFreestandingHelper(
        "_memhelper",
        "void *memcpy(void *d,const void *s,unsigned long n){\n"
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
        "  for(unsigned long i=0;i<n;i++)a[i]=(unsigned char)c;return d;}\n",
        EffTarget, IsARM32, ClangTargetOverride);
  }

  // Compile the ARM EABI conversions of a float or a double to a signed or
  // unsigned 64-bit integer, as compiler-rt defines them: truncation toward
  // zero, the nearest bound for a value out of range.  Integer operations
  // with constant shifts only, so the routines call no other run-time
  // routine and convert nothing themselves.
  std::string compileConversionHelper(const std::string &EffTarget,
                                      bool IsARM32,
                                      const std::string &ClangTargetOverride) {
    return compileFreestandingHelper(
        "_conversionhelper",
        "typedef unsigned long long u64;\n"
        "static u64 nd_fix(u64 m,int shift){\n"
        "  while(shift>0){m<<=1;--shift;}\n"
        "  while(shift<0){m>>=1;++shift;}\n"
        "  return m;}\n"
        "static u64 nd_double(double x){union{double d;u64 u;}v;v.d=x;"
        "return v.u;}\n"
        "static unsigned nd_float(float x){union{float f;unsigned u;}v;v.f=x;"
        "return v.u;}\n"
        "long long __aeabi_d2lz(double x){\n"
        "  u64 b=nd_double(x);int e=(int)((b>>52)&0x7ff)-1023;\n"
        "  int neg=(int)(b>>63);\n"
        "  if(e<0)return 0;\n"
        "  if(e>=63)return neg?(long long)(1ULL<<63):(long long)~(1ULL<<63);\n"
        "  u64 r=nd_fix((b&0xfffffffffffffULL)|0x10000000000000ULL,e-52);\n"
        "  return neg?(long long)(0-r):(long long)r;}\n"
        "u64 __aeabi_d2ulz(double x){\n"
        "  u64 b=nd_double(x);int e=(int)((b>>52)&0x7ff)-1023;\n"
        "  if(e<0||(b>>63))return 0;\n"
        "  if(e>=64)return ~0ULL;\n"
        "  return nd_fix((b&0xfffffffffffffULL)|0x10000000000000ULL,e-52);}\n"
        "long long __aeabi_f2lz(float x){\n"
        "  unsigned b=nd_float(x);int e=(int)((b>>23)&0xff)-127;\n"
        "  int neg=(int)(b>>31);\n"
        "  if(e<0)return 0;\n"
        "  if(e>=63)return neg?(long long)(1ULL<<63):(long long)~(1ULL<<63);\n"
        "  u64 r=nd_fix((u64)((b&0x7fffff)|0x800000),e-23);\n"
        "  return neg?(long long)(0-r):(long long)r;}\n"
        "u64 __aeabi_f2ulz(float x){\n"
        "  unsigned b=nd_float(x);int e=(int)((b>>23)&0xff)-127;\n"
        "  if(e<0||(b>>31))return 0;\n"
        "  if(e>=64)return ~0ULL;\n"
        "  return nd_fix((u64)((b&0x7fffff)|0x800000),e-23);}\n",
        EffTarget, IsARM32, ClangTargetOverride);
  }

  // Compile \p Source for the test target as a freestanding object named
  // \p Stem. Returns the object path, or "" on failure.
  std::string
  compileFreestandingHelper(const char *Stem, const std::string &Source,
                            const std::string &EffTarget, bool IsARM32,
                            const std::string &ClangTargetOverride) {
    auto SrcPath = (Work / (std::string(Stem) + ".c")).string();
    auto ObjPath = (Work / (std::string(Stem) + ".o")).string();
    {
      std::ofstream F(SrcPath);
      F << Source;
    }
    std::string Cmd =
        "clang -target " + EffTarget +
        " -nostdlib -c -O0 -fno-builtin -fno-stack-protector -fno-exceptions"
        " -fno-unwind-tables -fno-asynchronous-unwind-tables";
    if (IsARM32 && ClangTargetOverride.empty())
      Cmd += " -mcpu=cortex-a15 -mfloat-abi=softfp";
    else if (llvm::StringRef(EffTarget).starts_with("i386-") &&
             ClangTargetOverride.empty())
      Cmd += " -march=pentium4";
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

  LinkOutcome linkAndExtract(const std::string &ObjPath, const std::string &Tag,
                             uc_arch Arch, uc_mode Mode = UC_MODE_64) {
    LinkOutcome Result;
    // Resolve lazily: some originals need no link while their recompiled
    // objects do. Cache per process, without launching a probe for each case.
    // A found tool's execution failure is never evidence that it is absent.
    static const auto Linker = []() -> llvm::ErrorOr<std::string> {
      const char *SearchPath = std::getenv("PATH");
      if (!SearchPath)
        return std::make_error_code(std::errc::no_such_file_or_directory);
      llvm::SmallVector<llvm::StringRef, 16> Paths;
      llvm::StringRef(SearchPath).split(Paths, llvm::sys::EnvPathSeparator);
      for (auto &Path : Paths)
        if (Path.empty())
          Path = ".";
      // Explicit paths avoid Windows' additional implicit executable search
      // locations and keep availability tied to the configured tool PATH.
      return llvm::sys::findProgramByName("ld.lld", Paths);
    }();
    if (!Linker) {
      Result.State = LinkState::Unavailable;
      Result.Error =
          "ld.lld executable unavailable: " + Linker.getError().message();
      return Result;
    }
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
    std::string LinkCmd = neverd::test::shellQuote(*Linker) + " -m " + Emul +
                          " --image-base=" + HexBuf + " -Ttext=" + HexBuf +
                          " --oformat=elf -nostdlib --no-dynamic-linker"
                          " --noinhibit-exec -o " +
                          neverd::test::shellQuote(LinkedFile) + " " +
                          neverd::test::shellQuote(ObjPath);
    if (!MemHelperObj.empty())
      LinkCmd += " " + neverd::test::shellQuote(MemHelperObj);
    if (!ConversionHelperObj.empty())
      LinkCmd += " " + neverd::test::shellQuote(ConversionHelperObj);
    if (!LinkFirst.empty()) {
      const auto Order = (Work / (Tag + "_order.txt")).string();
      std::ofstream(Order) << LinkFirst << "\n";
      LinkCmd += " --symbol-ordering-file=" + neverd::test::shellQuote(Order);
    }
    Result.Command = LinkCmd;
    // Preserve bounded recovery from temporary process disruption, but retain
    // the final command result rather than assuming any failure is transient.
    for (int Attempt = 0; Attempt < kLinkRetries; ++Attempt) {
      Result.Execution = runCmd(LinkCmd);
      if (Result.Execution->ok())
        break;
      std::this_thread::sleep_for(
          std::chrono::milliseconds(30 * (Attempt + 1)));
    }
    if (!Result.Execution->ok()) {
      Result.Error =
          "linker failed after " + std::to_string(kLinkRetries) + " attempts";
      return Result;
    }

    auto LinkedBufOrErr = llvm::MemoryBuffer::getFile(LinkedFile);
    if (!LinkedBufOrErr) {
      Result.Error = "could not read linked image " + LinkedFile + ": " +
                     LinkedBufOrErr.getError().message();
      return Result;
    }
    auto LinkedObjOrErr = llvm::object::ObjectFile::createObjectFile(
        (*LinkedBufOrErr)->getMemBufferRef());
    if (!LinkedObjOrErr) {
      Result.Error = "could not parse linked image " + LinkedFile + ": " +
                     llvm::toString(LinkedObjOrErr.takeError());
      return Result;
    }

    captureSections(**LinkedObjOrErr, Result.Sections, /*AddrBias=*/CODE_BASE);
    if (Result.Sections.Text.empty()) {
      Result.Sections = {};
      Result.Error = "linked image has no executable text: " + LinkedFile;
      return Result;
    }
    Result.State = LinkState::Ready;
    return Result;
  }

  // Parse the SDK's owned bytes before deciding whether linking is required.
  // A failed write, parse or required link never falls back to relocatable
  // sections whose unresolved addresses cannot be emulated correctly.
  LinkOutcome extractSectionsWithLink(const unsigned char *Data, size_t Len,
                                      const std::string &Name) {
    LinkOutcome Result;
    auto Buffer = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<const char *>(Data), Len), "", false);
    auto ObjOrErr =
        llvm::object::ObjectFile::createObjectFile(Buffer->getMemBufferRef());
    if (!ObjOrErr) {
      Result.Error = "could not parse recompiled object: " +
                     llvm::toString(ObjOrErr.takeError());
      return Result;
    }
    SectionData PlainSD;
    captureSections(**ObjOrErr, PlainSD, /*AddrBias=*/0);
    if (!PlainSD.hasText()) {
      Result.Error = "recompiled object has no executable text";
      return Result;
    }

    uc_arch Arch = UC_ARCH_X86;
    uc_mode Mode = UC_MODE_64;
    auto ObjArch = (*ObjOrErr)->getArch();
    if (ObjArch == llvm::Triple::arm || ObjArch == llvm::Triple::armeb)
      Arch = UC_ARCH_ARM;
    else if (ObjArch == llvm::Triple::aarch64)
      Arch = UC_ARCH_ARM64;
    else if (ObjArch == llvm::Triple::x86)
      Mode = UC_MODE_32;

    if (PlainSD.requiresLink() || objectNeedsLink(**ObjOrErr)) {
      // Split `.text.*` is a required-link input even without data or explicit
      // relocations: only the linker owns its placement relative to the entry
      // image.  A successful link that still exposes no executable text is an
      // invalid roundtrip result, not a reason to skip execution.
      auto ObjFile = (Work / (Name + "_recomp.o")).string();
      {
        std::ofstream F(ObjFile, std::ios::binary);
        F.write(reinterpret_cast<const char *>(Data), Len);
        F.close();
        if (!F) {
          Result.Error = "could not write recompiled object: " + ObjFile;
          return Result;
        }
      }
      return linkAndExtract(ObjFile, Name + "_recomp", Arch, Mode);
    }
    Result.State = LinkState::Ready;
    Result.Sections = std::move(PlainSD);
    return Result;
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
