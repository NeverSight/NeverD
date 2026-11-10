//===- X86LegacySourceTests.cpp - Compiled protected dispatch ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile real exported C, compare original branches and exercise SDK/CLI.
//===----------------------------------------------------------------------===//

#include "../../HostCpu.h"
#include "../../lift/NeverDLiftFixture.h"
#include "X86BranchDebugOracle.h"
#include "X86LegacyBMITestEncoding.h"
#include "X86LegacyControlTestEncoding.h"
#include "X86LegacyDescriptorTestEncoding.h"
#include "X86LegacyMachineStatusTestEncoding.h"
#include "X86LegacySystemSelectorTestEncoding.h"
#include "X86LegacyVM86TestState.h"
#include "X86PortDevice.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/X86LegacySourceExport.h"
#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/translate/X86/X86LegacySourceExecution.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::translate;

namespace {

std::string readSourceArtifact(const fs::path &Path) {
  std::ifstream File(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(File), {}};
}

struct SourceUnit {
  uint32_t Address;
  unsigned Bits;
  std::vector<uint8_t> Bytes;
};

BinaryImage image(llvm::ArrayRef<SourceUnit> Units) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Base = Image.Entry = 0x1000;
  Segment S;
  S.Name = ".text";
  S.VA = 0x1000;
  S.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  size_t Size = 0x2000;
  for (const auto &U : Units)
    Size = std::max(Size, size_t(U.Address - S.VA) + U.Bytes.size());
  S.Data.resize(Size, 0xcc);
  S.Size = S.FileSz = S.Data.size();
  for (const auto &U : Units)
    std::copy(U.Bytes.begin(), U.Bytes.end(),
              S.Data.begin() + U.Address - S.VA);
  Image.Segments.push_back(std::move(S));
  return Image;
}

std::string request(llvm::ArrayRef<SourceUnit> Units) {
  llvm::json::Array Rows;
  for (const auto &U : Units)
    Rows.push_back(llvm::json::Object{{"address", U.Address},
                                      {"code_bits", U.Bits},
                                      {"size", U.Bytes.size()}});
  return llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                  {"profile", "protected-legacy-v1"},
                                  {"instructions", std::move(Rows)}}))
      .str();
}

TEST(X86LegacySourceContract, RejectsMalformedAndUnsupportedSelections) {
  const auto Image = image({{0x1000, 32, {0xb8, 1, 2, 3, 4}}});
  const auto Reject = [&](llvm::StringRef Text) {
    auto Got = exportX86LegacySource(Image, Text);
    EXPECT_FALSE(bool(Got)) << Text.str();
    if (!Got)
      llvm::consumeError(Got.takeError());
  };
  for (
      const auto *Text :
      {"", "null", "[]", "{}",
       R"({"profile":"protected-legacy-v1","instructions":[]})",
       R"({"profile":"protected-legacy-v1","instructions":[null]})",
       R"({"profile":"protected-legacy-v1","instructions":[{"address":4096,"code_bits":64,"size":5}]})",
       R"({"profile":"protected-legacy-v1","instructions":[{"address":4096,"code_bits":32,"size":3}]})",
       R"({"profile":"protected-legacy-v1","instructions":[{"address":4096,"code_bits":32,"size":6}]})",
       R"({"profile":"protected-legacy-v1","instructions":[{"address":-1,"code_bits":32,"size":1}]})",
       R"({"profile":"protected-legacy-v1","instructions":[{"address":"0x100000000","code_bits":32,"size":1}]})",
       R"({"profile":"protected-legacy-v1","instructions":[{"address":12288,"code_bits":32,"size":1}]})",
       R"({"profile":"protected-legacy-v1","instructions":[{"address":4096,"code_bits":32,"size":5,"bytes":"90"}]})",
       R"({"profile":"protected-legacy-v1","instructions":[{"address":4096,"code_bits":32,"size":5}],"guess":true})"})
    Reject(Text);
  Reject(
      request({{0x1000, 32, {1, 2, 3, 4, 5}}, {0x1000, 32, {1, 2, 3, 4, 5}}}));
  for (const std::vector<uint8_t> Bytes : {std::vector<uint8_t>{0x0f, 0xa2},
                                           {0x90, 0x90},
                                           {0x0f, 0x21, 0xc0, 0}}) {
    const std::vector<SourceUnit> Units{{0x1000, 32, Bytes}};
    auto Got = exportX86LegacySource(image(Units), request(Units));
    EXPECT_FALSE(bool(Got));
    if (!Got)
      llvm::consumeError(Got.takeError());
  }
  auto WrongArch = Image;
  WrongArch.Arch = Arch::X64;
  auto Got = exportX86LegacySource(WrongArch, request({{0x1000, 32, {0x90}}}));
  EXPECT_FALSE(bool(Got));
  llvm::consumeError(Got.takeError());
}

class X86LegacySourceTest : public NeverDLiftTest,
                            public X86LegacyVM86TestState,
                            public testing::WithParamInterface<unsigned> {
protected:
  using Driver = uint32_t (*)(void *, uint64_t *);
  using Select = decltype(&neverd_x86_legacy_select_v1);
  using Step = decltype(&neverd_x86_legacy_step_v1);
  using Attempted = decltype(&neverd_x86_legacy_attempted_v1);
  using Pending = decltype(&neverd_x86_legacy_pending_v1);
  using Bind = void (*)(Select, Step, Attempted, Pending);
  std::string Compiler;
  unsigned Compilation = 0;

  void SetUp() override {
#if !defined(__linux__) || !defined(__x86_64__)
    GTEST_SKIP() << "Compiled legacy source currently qualifies Linux x64";
#else
    NeverDLiftTest::SetUp();
    resetLegacyState();
    Compiler = NEVERD_TEST_CLANG;
    if (GetParam() & 2) {
      const auto *NeverC = std::getenv("NEVERD_TEST_NEVERC");
      if (!NeverC || !*NeverC)
        GTEST_SKIP() << "NEVERD_TEST_NEVERC is required for this compiler lane";
      Compiler = NeverC;
    }
#endif
  }

  void install(llvm::ArrayRef<SourceUnit> Units) {
    for (const auto &U : Units)
      for (unsigned I = 0; I != U.Bytes.size(); ++I)
        put(U.Address + I, U.Bytes[I], GuestScalarWidth::I8);
  }

  void controlState(bool PAE = false, bool Code32 = true) {
    resetLegacyState({.PhysicalAddressBits = 32, .PSE = true, .PAE = true});
    S.Core.CPL = 0;
    S.Core.Segments[1] = {0, UINT32_MAX, 8, uint16_t(Code32 ? 0xc09b : 0x809b),
                          false};
    S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
    S.EFlags = 0xed7;
    O.ControlRegisters = X86LegacyControlRegisterBinding{0x88};
    auto C = M->controls();
    C.CR0 = 0x80010011;
    C.CR4 = PAE ? 0x20 : 0;
    if (PAE) {
      C.PDPTE = {0x11001, 0, 0, 0};
      put(0x10000, 0x11001);
      put(0x11000, 0x12007);
      for (unsigned I = 0; I != 32; ++I)
        put(0x12000 + I * 8, I * 4096 | 7);
    }
    llvm::cantFail(M->bindControls(C));
  }

  void compile(llvm::ArrayRef<SourceUnit> Units, Driver &Run) {
    auto Export = exportX86LegacySource(image(Units), request(Units));
    ASSERT_TRUE(bool(Export)) << llvm::toString(Export.takeError());
    std::string Name =
        testing::UnitTest::GetInstance()->current_test_info()->name();
    std::replace(Name.begin(), Name.end(), '/', '_');
    Name += "-" + std::to_string(Compilation++);
    const auto Source = tmpFile(Name + ".c"), Library = tmpFile(Name + ".so");
    std::ofstream File(Source);
    File << Export->Source << R"C(
static uint32_t (*select_bridge)(void *, uint64_t *);
static uint32_t (*step_bridge)(void *, uint64_t, const uint8_t *, uint32_t);
static uint32_t (*attempted_bridge)(void *);
static uint32_t (*pending_bridge)(void *);
void bind_legacy(uint32_t (*select)(void *, uint64_t *),
    uint32_t (*step)(void *, uint64_t, const uint8_t *, uint32_t),
    uint32_t (*attempted)(void *), uint32_t (*pending)(void *)) {
    select_bridge = select; step_bridge = step; attempted_bridge = attempted;
    pending_bridge = pending;
}
uint32_t neverd_x86_legacy_select_v1(void *context, uint64_t *key) {
    return select_bridge(context, key);
}
uint32_t neverd_x86_legacy_step_v1(void *context, uint64_t key,
    const uint8_t *bytes, uint32_t size) {
    return step_bridge(context, key, bytes, size);
}
uint32_t neverd_x86_legacy_attempted_v1(void *context) {
    return attempted_bridge(context);
}
uint32_t neverd_x86_legacy_pending_v1(void *context) {
    return pending_bridge(context);
}
)C";
    File.close();
    if (const auto *Artifacts =
            std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      fs::create_directories(Artifacts);
      fs::copy_file(Source, fs::path(Artifacts) / (Name + ".c"),
                    fs::copy_options::overwrite_existing);
      std::ofstream(fs::path(Artifacts) / (Name + ".generated.c"))
          << Export->Source;
      std::ofstream(fs::path(Artifacts) / (Name + ".json")) << Export->Manifest;
    }
    auto Built =
        exec(Compiler, {"-std=c11", GetParam() & 1 ? "-O2" : "-O0", "-fPIC",
                        "-shared", "-Wl,-Bsymbolic", "-Wall", "-Wextra",
                        "-Werror", Source.string(), "-o", Library.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    if (const auto *Artifacts =
            std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR"))
      fs::copy_file(Library, fs::path(Artifacts) / (Name + ".so"),
                    fs::copy_options::overwrite_existing);
    std::string Error;
    auto DL = llvm::sys::DynamicLibrary::getPermanentLibrary(
        Library.string().c_str(), &Error);
    ASSERT_TRUE(DL.isValid()) << Error;
    auto Binder = reinterpret_cast<Bind>(DL.getAddressOfSymbol("bind_legacy"));
    ASSERT_NE(Binder, nullptr);
    Binder(&neverd_x86_legacy_select_v1, &neverd_x86_legacy_step_v1,
           &neverd_x86_legacy_attempted_v1, &neverd_x86_legacy_pending_v1);
    Run = reinterpret_cast<Driver>(
        DL.getAddressOfSymbol("neverd_x86_legacy_continue_v1"));
    ASSERT_NE(Run, nullptr);
    install(Units);
  }
};

#include "X86LegacyHaltSourceTests.inc"
#include "X86LegacyMpxSourceTests.inc"
#include "X86LegacyTaskSourceTests.inc"
#include "X86LegacyVM86SourceTests.inc"

TEST_P(X86LegacySourceTest, ModesAliasesAndBudgetsUseLiveCaches) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      compile({{0x1000, 16, {0xb8, 0x34, 0x12}},
               {0x1000, 32, {0xb8, 0x34, 0x12, 0x78, 0x56}}},
              Run));
  for (bool Code32 : {false, true})
    for (uint32_t Base : {0x800u, 0xffffff00u, 0xffff1001u}) {
      resetLegacyState();
      put(0x1000, 0x56781234b8ULL);
      auto &CS = S.Core.Segments[1];
      CS.Base = Base;
      if (!Code32)
        CS.Attributes &= ~0x4000;
      S.EIP = uint32_t(0x1000 - Base);
      const auto Entry = S.EIP;
      S.Core.GPR[0] = 0xdeadbeef;
      X86LegacySourceExecution Context(S, *M, O);
      uint64_t Budget = 1;
      ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
      EXPECT_EQ(Budget, 0u);
      EXPECT_EQ(S.EIP, Entry + (Code32 ? 5 : 3));
      EXPECT_EQ(S.Core.GPR[0], Code32 ? 0x56781234u : 0xdead1234u);
      EXPECT_TRUE(Context.lastResult().InstructionCompleted);
      Budget = 2;
      EXPECT_EQ(Run(&Context, &Budget), 257u);
      EXPECT_EQ(Budget, 2u);
      S.EFlags = 0;
      // A stale resource receipt without an instruction journal grants nothing.
      S.PendingInstructionAttempted = true;
      Budget = 0;
      EXPECT_EQ(Run(&Context, &Budget), 258u);
      Budget = 1;
      EXPECT_EQ(Run(&Context, &Budget), 256u);
      EXPECT_FALSE(Context.lastError().empty());
      EXPECT_EQ(Budget, 1u);
    }
  uint64_t Budget = 1;
  EXPECT_EQ(Run(nullptr, &Budget), 256u);
  EXPECT_EQ(neverd_x86_legacy_pending_v1(nullptr), 0u);
  X86LegacySourceExecution Context(S, *M, O);
  EXPECT_EQ(Run(&Context, nullptr), 256u);
}

TEST_P(X86LegacySourceTest, UnselectedCodeModeDoesNotFetchOrSpendBudget) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 16, {0x90}}}, Run));
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 2;
  EXPECT_EQ(Run(&Context, &Budget), 257u);
  EXPECT_EQ(Budget, 2u);
  EXPECT_EQ(get(0x11004), 0x1007u);
  S.Core.Segments[1].Attributes &= ~0x4000;
  EXPECT_EQ(Run(&Context, &Budget), 257u);
  EXPECT_EQ(Budget, 1u);
  EXPECT_EQ(S.EIP, 0x1001u);
  EXPECT_TRUE(Context.lastResult().InstructionCompleted);
}

TEST_P(X86LegacySourceTest, OriginalHandlerRepairsPageAndReturnsToRetry) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      compile({{0x1000, 32, {0x8b, 0x07}},
               {0x2000, 32, {0xc7, 0x05, 0x34, 0x10, 1, 0, 7, 0xd0, 0, 0}},
               {0x200a, 32, {0x83, 0xc4, 4}},
               {0x200d, 32, {0xcf}}},
              Run));
  put(0x5804, 0xb000, GuestScalarWidth::I32);
  put(0xd004, 0x89abcdef, GuestScalarWidth::I32);
  put(0x11034, 0xd006, GuestScalarWidth::I32);
  S.Core.GPR[7] = 0xd004;
  const auto Original = S;
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 5;
  ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
  EXPECT_EQ(Budget, 4u);
  EXPECT_EQ(S.EIP, 0x2000u);
  EXPECT_EQ(Context.lastResult().DeliveredVector, 14);
  EXPECT_FALSE(Context.lastResult().InstructionCompleted);
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.EIP, 0x1002u);
  EXPECT_EQ(S.Core.GPR[0], 0x89abcdefu);
  EXPECT_EQ(S.Core.GPR[4], Original.Core.GPR[4]);
  EXPECT_EQ(S.Core.CPL, Original.Core.CPL);
  EXPECT_EQ(S.EFlags, Original.EFlags & ~0x10000u);
  EXPECT_EQ(S.CR2, 0xd004u);
}

TEST_P(X86LegacySourceTest, StackFramesRoundTripAtBothOperandWidths) {
  for (bool Word : {false, true}) {
    resetLegacyState();
    std::vector<SourceUnit> Units;
    uint32_t PC = 0x1000;
    for (std::vector<uint8_t> Bytes :
         {std::vector<uint8_t>{0x60}, {0x61}, {0xc8, 0x20, 0, 3}, {0xc9}}) {
      if (Word)
        Bytes.insert(Bytes.begin(), 0x66);
      Units.push_back({PC, 32, Bytes});
      PC += Bytes.size();
    }
    Driver Run = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
    S.Core.GPR = {0x11223344, 0x22334455, 0x33445566, 0x44556677,
                  0xd100,     0xc100,     0x66778899, 0x778899aa};
    const auto Before = S.Core.GPR;
    X86LegacySourceExecution Context(S, *M, O);
    uint64_t Budget = 4;
    ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
    EXPECT_EQ(S.EIP, PC);
    EXPECT_EQ(S.Core.GPR, Before);
    EXPECT_EQ(S.EFlags, 0x202u);
    EXPECT_EQ(Budget, 0u);
    EXPECT_FALSE(S.PendingStack);
  }
}

TEST_P(X86LegacySourceTest, StackBackingContinuationRetainsAliasedCodeAndRAM) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0xd00c, 32, {0xc8, 0, 0, 4}}}, Run));
  S.EIP = 0xd00c;
  S.Core.GPR[4] = 0xd010;
  S.Core.GPR[5] = 0xd008;
  put(0xd004, 0x11223344, GuestScalarWidth::I32);
  put(0xd000, 0x55667788, GuestScalarWidth::I32);
  put(0xcffc, 0x99aabbcc, GuestScalarWidth::I32);
  put(0x11030, 0x40007, GuestScalarWidth::I32);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 4u) << Context.lastError().str();
  ASSERT_TRUE(S.PendingStack);
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(get(0xd004), 0x55667788u);
  EXPECT_EQ(get(0xd00c), 0xd008u);
  const auto Before = M->physicalMemory().snapshotMemoryRegions().front().Bytes;
  EXPECT_EQ(Run(&Context, &Budget), 4u);
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes, Before);
  auto Rejected = executeX86LegacyInstruction(S, *M, {0x90}, O);
  ASSERT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
  EXPECT_TRUE(S.PendingStack);
  EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes, Before);
  auto UnboundOptions = O;
  UnboundOptions.Scalars.reset();
  X86LegacySourceExecution Unbound(S, *M, UnboundOptions);
  EXPECT_EQ(Run(&Unbound, &Budget),
            uint32_t(X86LegacySourceExit::InvalidState));
  EXPECT_EQ(Budget, 0u);
  EXPECT_TRUE(S.PendingStack);
  EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes, Before);
  auto RAM = llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  RAM.Memory = M->physicalMemory().snapshotMemoryRegions();
  RAM.Memory.push_back(
      {0x40000, MemoryPermission::Read | MemoryPermission::Write, 0,
       std::vector<uint8_t>(Before.begin() + 0xc000, Before.begin() + 0xd000)});
  M = llvm::cantFail(X86PagedMemory::create(RAM, M->features(), M->controls()));
  X86LegacySourceExecution Restored(S, *M, O);
  ASSERT_EQ(Run(&Restored, &Budget), 258u) << Restored.lastError().str();
  EXPECT_EQ(Budget, 0u); // Resuming one attempted instruction is not another.
  EXPECT_TRUE(Restored.lastResult().InstructionCompleted);
  EXPECT_FALSE(Restored.lastResult().InstructionAttempted);
  EXPECT_FALSE(S.PendingStack);
  EXPECT_EQ(get(0xd008), 0x11223344u);
  EXPECT_EQ(get(0xd004), 0x55667788u);
  EXPECT_EQ(get(0xd000), 0x99aabbccu);
  EXPECT_EQ(get(0x40ffc), 0xd00cu);
  EXPECT_EQ(S.Core.GPR[4], 0xcffcu);
  EXPECT_EQ(S.Core.GPR[5], 0xd00cu);
}

TEST_P(X86LegacySourceTest, UnchargedOwnerErrorRequiresBudgetBeforeResume) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    SCOPED_TRACE(Case);
    const bool Mpx = Case == 2;
    resetLegacyState();
    Driver Run = nullptr;
    const std::vector<uint8_t> Bytes =
        Mpx ? std::vector<uint8_t>{0x66, 0x0f, 0x1a, 0x03}
            : std::vector<uint8_t>{0x60};
    ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, Bytes}}, Run));
    if (Mpx) {
      bindMpx();
      S.Core.Segments[3].Base = 0x4000;
      S.Core.GPR[3] = 0xfffffffc;
      put(0x3ffc, 0x2222222211111111ULL);
    } else {
      S.Core.Segments[2].Base = 0x4000;
      S.Core.GPR[4] = 1 + Case * 4;
    }
    O.Segments.FullLimitChoice.reset();
    X86LegacySourceExecution Context(S, *M, O);
    uint64_t Budget = 1;
    ASSERT_EQ(Run(&Context, &Budget),
              uint32_t(X86LegacySourceExit::InvalidState));
    EXPECT_FALSE(Context.lastResult().InstructionAttempted);
    EXPECT_EQ(Budget, 1u);
    ASSERT_TRUE(Mpx ? S.PendingMpx.has_value() : S.PendingStack.has_value());
    if (!Mpx) {
      EXPECT_EQ(S.PendingStack->CompletedAccesses.size(), Case);
      if (Case == 1) {
        EXPECT_EQ(get(0x4001), 35u);
        put(0x4001, 0xcafebabe, GuestScalarWidth::I32);
      }
    }
    const auto Before =
        M->physicalMemory().snapshotMemoryRegions().front().Bytes;
    O.Segments.FullLimitChoice = X86FullSegmentLimitChoice::Wrap;
    X86LegacySourceExecution Restored(S, *M, O);
    Budget = 0;
    EXPECT_EQ(Run(&Restored, &Budget),
              uint32_t(X86LegacySourceExit::BudgetExhausted));
    EXPECT_EQ(S.EIP, 0x1000u);
    EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes,
              Before);
    if (Case == 0) {
      put(0x1100c, 0x40007, GuestScalarWidth::I32);
      Budget = 1;
      ASSERT_EQ(Run(&Restored, &Budget),
                uint32_t(X86LegacyExecutionStatus::BackingFault))
          << Restored.lastError().str();
      EXPECT_EQ(Budget, 0u);
      EXPECT_TRUE(Restored.lastResult().InstructionAttempted);
      ASSERT_TRUE(S.PendingStack);
      auto RAM =
          llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
      RAM.Memory = M->physicalMemory().snapshotMemoryRegions();
      RAM.Memory.push_back({0x40000,
                            MemoryPermission::Read | MemoryPermission::Write, 0,
                            std::vector<uint8_t>(Before.begin() + 0x3000,
                                                 Before.begin() + 0x4000)});
      M = llvm::cantFail(
          X86PagedMemory::create(RAM, M->features(), M->controls()));
      put(0x1000, 0xcc, GuestScalarWidth::I8);
    }
    X86LegacySourceExecution Completed(S, *M, O);
    Budget = Case == 0 ? 0 : 1;
    EXPECT_EQ(Run(&Completed, &Budget),
              uint32_t(X86LegacySourceExit::BudgetExhausted))
        << Completed.lastError().str();
    EXPECT_EQ(Budget, 0u);
    EXPECT_EQ(Completed.lastResult().InstructionAttempted, Case != 0);
    EXPECT_TRUE(Completed.lastResult().InstructionCompleted);
    EXPECT_EQ(S.EIP, 0x1000u + Bytes.size());
    EXPECT_FALSE(S.PendingStack);
    EXPECT_FALSE(S.PendingMpx);
    if (Mpx)
      EXPECT_EQ(*S.Mpx->bounds(0), (X86MpxBounds{{0x11111111, UINT32_MAX},
                                                 {0x22222222, UINT32_MAX}}));
    else
      EXPECT_EQ(S.Core.GPR[4], 0xffffffe1u + Case * 4);
    if (Case == 1)
      EXPECT_EQ(get(0x4001), 0xcafebabeu);
  }
}

TEST_P(X86LegacySourceTest, PartialPopAllFaultHandlerRepairsAndRetries) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      compile({{0x1000, 32, {0x61}},
               {0x2000, 32, {0xc7, 0x05, 0x34, 0x10, 1, 0, 7, 0xd0, 0, 0}},
               {0x200a, 32, {0x83, 0xc4, 4}},
               {0x200d, 32, {0xcf}}},
              Run));
  S.Core.GPR[4] = 0xcff0;
  put(0x5804, 0xb000, GuestScalarWidth::I32);
  for (unsigned I = 0; I != 8; ++I)
    put(0xcff0 + I * 4, 0x12340000 + I, GuestScalarWidth::I32);
  put(0x11034, 0xd006, GuestScalarWidth::I32);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 6;
  ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
  EXPECT_EQ(Context.lastResult().DeliveredVector, 14);
  EXPECT_EQ(S.Core.GPR[7], 0x12340000u);
  EXPECT_EQ(S.Core.GPR[5], 0x12340002u);
  EXPECT_FALSE(S.PendingStack);
  ASSERT_EQ(Run(&Context, &Budget), 257u) << Context.lastError().str();
  EXPECT_EQ(S.EIP, 0x1001u);
  EXPECT_EQ(S.Core.GPR[4], 0xd010u);
  EXPECT_EQ(S.Core.GPR[0], 0x12340007u);
  EXPECT_EQ(S.Core.CPL, 3);
  EXPECT_EQ(Budget, 1u);
}

TEST_P(X86LegacySourceTest, FarCallAndReturnRedispatchAcrossCodeModes) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x9a, 0, 0x20, 0, 0, 0x33, 0}},
                                   {0x1007, 32, {0x90}},
                                   {0x2000, 16, {0xb8, 0x34, 0x12}},
                                   {0x2003, 16, {0x66, 0xcb}}},
                                  Run));
  put(0x7030, 0x0000fb000000ffffULL);
  O.FarTransfers = X86LegacyFarTransferProfile::OrderedFrame;
  S.Core.GPR[0] = 0xdeadbeef;
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(S.EIP, 0x2000u);
  EXPECT_EQ(S.Core.Segments[1].Selector, 0x33);
  EXPECT_EQ(S.Core.Segments[1].Attributes & 0x4000, 0u);
  EXPECT_EQ(S.Core.GPR[4], 0xcff8u);
  Budget = 3;
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(S.EIP, 0x1008u);
  EXPECT_EQ(S.Core.Segments[1].Selector, 0x1b);
  EXPECT_NE(S.Core.Segments[1].Attributes & 0x4000, 0u);
  EXPECT_EQ(S.Core.GPR[4], 0xd000u);
  EXPECT_EQ(S.Core.GPR[0], 0xdead1234u);
}

TEST_P(X86LegacySourceTest, IntegerCountsAndIndependentCarryChains) {
  for (bool Code32 : {false, true}) {
    resetLegacyState();
    O.IntegerExtensions = {true, true, true, true};
    O.BitCountUndefinedFlags = 0x810;
    S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
    std::vector<SourceUnit> Units;
    uint32_t Address = 0x1000;
    for (auto Bytes : {std::vector<uint8_t>{0xf3, 0x0f, 0xb8, 0xc1},
                       {0xf3, 0x0f, 0xbc, 0xc1},
                       {0xf3, 0x0f, 0xbd, 0xc1},
                       {0x66, 0x0f, 0x38, 0xf6, 0xc1},
                       {0xf3, 0x0f, 0x38, 0xf6, 0xc1}}) {
      Units.push_back({Address, Code32 ? 32u : 16u, Bytes});
      Address += Bytes.size();
    }
    Driver Run = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
    X86LegacySourceExecution Context(S, *M, O);
    for (unsigned I = 0; I != Units.size(); ++I) {
      S.Core.GPR[0] = 0xffffffff;
      S.Core.GPR[1] = I < 3 ? 0x80000001 : 1;
      S.EFlags = 0xed7;
      uint64_t Budget = 1;
      ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
      EXPECT_EQ(Budget, 0u);
      const uint32_t Counts[]{Code32 ? 2u : 1u, 0, Code32 ? 0u : 15u};
      const uint32_t Expected =
          I < 3 ? (Code32 ? 0 : 0xffff0000) | Counts[I] : 1;
      EXPECT_EQ(S.Core.GPR[0], Expected);
      const uint32_t Flags =
          I < 3 ? 0x602 | (I ? 0x810 : 0) | (!Counts[I] ? 64 : 0) : 0xed7;
      EXPECT_EQ(S.EFlags, Flags);
    }
  }
}

TEST_P(X86LegacySourceTest, IntegerCountFaultHandlerRepairsAndRetriesOriginal) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      compile({{0x1000, 32, {0xf3, 0x0f, 0xb8, 0x07}},
               {0x1004, 32, {0x90}},
               {0x2000, 32, {0xc7, 0x05, 0x24, 0x10, 1, 0, 7, 0x90, 0, 0}},
               {0x200a, 32, {0x83, 0xc4, 4}},
               {0x200d, 32, {0xcf}}},
              Run));
  O.IntegerExtensions = {true, true, true, true};
  S.Core.GPR[0] = 0x12345678;
  S.Core.GPR[7] = 0x9000;
  put(0x9000, 0x8101, GuestScalarWidth::I32);
  put(0x11024, 0, GuestScalarWidth::I32);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
  EXPECT_EQ(S.CR2, 0x9000u);
  EXPECT_EQ(Context.lastResult().DeliveredVector, 14);
  EXPECT_EQ(Budget, 0u);
  Budget = 5;
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(S.Core.GPR[0], 3u);
  EXPECT_EQ(S.EIP, 0x1005u);
  EXPECT_EQ(S.Core.CPL, 3);
}

TEST_P(X86LegacySourceTest, IntegerFeatureBindingsSelectAliasesOrGuestUD) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0xf3, 0x0f, 0xbc, 0xc1}},
                                   {0x1004, 32, {0xf3, 0x0f, 0xb8, 0xc1}}},
                                  Run));
  S.Core.GPR[1] = 0;
  uint64_t Budget = 3;
  X86LegacySourceExecution Unbound(S, *M, O);
  EXPECT_EQ(Run(&Unbound, &Budget), 256u);
  EXPECT_EQ(Budget, 3u);
  O.IntegerExtensions.emplace();
  O.BitScanChoices = {0x98765432, 0};
  X86LegacySourceExecution Bound(S, *M, O);
  ASSERT_EQ(Run(&Bound, &Budget), 1u) << Bound.lastError().str();
  EXPECT_EQ(Budget, 1u);
  EXPECT_EQ(S.Core.GPR[0], 0x98765432u);
  EXPECT_EQ(Bound.lastResult().DeliveredVector, 6);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1004u);
}

TEST_P(X86LegacySourceTest, MachineStatusChangesReachFollowingInstructions) {
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true}) {
      resetLegacyState();
      S.Core.CPL = 0;
      S.Core.Segments[1] = {0, UINT32_MAX, 8,
                            uint16_t(Code32 ? 0xc09b : 0x809b), false};
      S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
      S.EFlags = 0xed7;
      auto Controls = M->controls();
      Controls.CR0 = 0x8005001b;
      llvm::cantFail(M->bindControls(Controls));
      O.MachineStatus = X86LegacyMachineStatusProfile::FullCR0;
      S.Core.GPR[0] = S.Core.GPR[2] = 0x89abcdef;
      S.Core.GPR[3] = 0x9000;
      S.Core.GPR[6] = 0xfffffffc;
      put(0x9000, 0xdeadbeef, GuestScalarWidth::I32);
      std::vector<SourceUnit> Units;
      uint32_t PC = 0x1000;
      for (const auto &[Form, Register] :
           {std::pair{1u, 6}, {0u, 0}, {2u, 6}, {0u, 2}, {0u, -1}}) {
        auto Bytes = test::legacyMachineStatusBytes(Form, Code32, Operand32,
                                                    true, Register);
        Units.push_back({PC, Code32 ? 32u : 16u, Bytes});
        PC += Bytes.size();
      }
      Driver Run = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
      X86LegacySourceExecution Context(S, *M, O);
      uint64_t Budget = Units.size();
      ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
      EXPECT_EQ(Budget, 0u);
      EXPECT_EQ(S.EIP, PC);
      EXPECT_EQ(S.Core.GPR[0], Operand32 ? 0x8005001du : 0x89ab001du);
      EXPECT_EQ(S.Core.GPR[2], Operand32 ? 0x80050015u : 0x89ab0015u);
      EXPECT_EQ(M->controls().CR0, 0x80050015u);
      EXPECT_EQ(get(0x9000), 0xdead0015u);
      EXPECT_EQ(S.EFlags, 0xed7u);
    }
}

TEST_P(X86LegacySourceTest, MachineStatusProfileRefusesBeforeFetch) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 1, 0xe0}}}, Run));
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  EXPECT_EQ(Run(&Context, &Budget), 256u);
  EXPECT_EQ(Budget, 1u);
  EXPECT_EQ(get(0x11004), 0x1007u);
  EXPECT_EQ(M->controls().CR0, 0x80010001u);
}

TEST_P(X86LegacySourceTest, MachineStatusFaultRepairAndRetry) {
  for (unsigned Form : {0u, 1u}) {
    resetLegacyState();
    Driver Run = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        compile({{0x1000, 32, {0x0f, 1, uint8_t(Form == 0 ? 0x23 : 0x33)}},
                 {0x1003, 32, {0x90}},
                 {0x2000, 32, {0xc7, 0x05, 0x20, 0x10, 1, 0, 7, 0x80, 0, 0}},
                 {0x200a, 32, {0x83, 0xc4, 4}},
                 {0x200d, 32, {0xcf}}},
                Run));
    S.Core.CPL = 0;
    S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
    S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
    S.Core.GPR[3] = 0x8000;
    put(0x8000, 0xdeadfffe, GuestScalarWidth::I32);
    put(0x11020, 0, GuestScalarWidth::I32);
    X86LegacySourceExecution Context(S, *M, O);
    uint64_t Budget = 1;
    ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
    EXPECT_EQ(Context.lastResult().DeliveredVector, 14);
    EXPECT_EQ(M->controls().CR0, 0x80010001u);
    EXPECT_EQ(S.CR2, 0x8000u);
    EXPECT_EQ(get(S.Core.GPR[4]), Form == 0 ? 2u : 0u);
    Budget = 5;
    ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
    EXPECT_EQ(M->controls().CR0, Form == 0 ? 0x80010001u : 0x8001000fu);
    EXPECT_EQ(S.EIP, 0x1004u);
    EXPECT_EQ(get(0x8000), Form == 0 ? 0xdead0001u : 0xdeadfffeu);
  }
}

TEST_P(X86LegacySourceTest, MachineStatusCommitsBeforeDebugTrap) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 6}}}, Run));
  S.Core.CPL = 0;
  S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
  S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
  S.EFlags = 0x302;
  O.Debug = X86LegacyDebugProfile::InstructionStep;
  S.Debug.emplace();
  auto Controls = M->controls();
  Controls.CR0 = 0x8001000b;
  llvm::cantFail(M->bindControls(Controls));
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
  EXPECT_EQ(Context.lastResult().DeliveredVector, 1);
  EXPECT_TRUE(Context.lastResult().InstructionCompleted);
  EXPECT_EQ(M->controls().CR0, 0x80010003u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1002u);
}

TEST_P(X86LegacySourceTest, MachineStatusAccessControlChangesSMAPPermission) {
  resetLegacyState({.PhysicalAddressBits = 32, .SMAP = true});
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 1, 0xcb}},
                                   {0x1003, 32, {0x8b, 3}},
                                   {0x1005, 32, {0x0f, 1, 0xca}},
                                   {0x1008, 32, {0x8b, 3}}},
                                  Run));
  S.Core.CPL = 0;
  S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
  S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
  S.Core.GPR[3] = 0x9000;
  put(0x9000, 0x89abcdef, GuestScalarWidth::I32);
  for (unsigned Page = 0; Page != 32; ++Page)
    put(0x11000 + Page * 4, Page * 4096 | (Page == 9 ? 7 : 3),
        GuestScalarWidth::I32);
  auto Controls = M->controls();
  Controls.CR4 = 1u << 21;
  llvm::cantFail(M->bindControls(Controls));
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 4;
  ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.Core.GPR[0], 0x89abcdefu);
  EXPECT_EQ(Context.lastResult().DeliveredVector, 14);
  EXPECT_EQ(S.CR2, 0x9000u);
  EXPECT_EQ(get(S.Core.GPR[4]), 1u);
  EXPECT_EQ(get(S.Core.GPR[4] + 4), 0x1008u);
}

TEST_P(X86LegacySourceTest, MachineStatusDataTrapObservesCommittedControl) {
  const SourceUnit Instruction{0x1000, 32, {0x0f, 1, 0x33}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({Instruction}, Run));
  for (bool Fault : {false, true})
    for (bool TF : {false, true}) {
      SCOPED_TRACE(testing::Message() << "fault=" << Fault << " TF=" << TF);
      resetLegacyState();
      install({Instruction});
      S.Core.CPL = 0;
      S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
      S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
      S.EFlags = TF ? 0x302 : 0x202;
      S.Core.GPR[3] = 0x9000;
      put(0x9000, 0xdeadfff4, GuestScalarWidth::I32);
      O.Debug = X86LegacyDebugProfile::Watchpoints;
      S.Debug.emplace();
      S.Debug->DR[0] = 0x9001;
      S.Debug->DR7 = 0x30401;
      auto Controls = M->controls();
      Controls.CR0 = 0x8005001f;
      llvm::cantFail(M->bindControls(Controls));
      if (Fault)
        put(0x11024, 0, GuestScalarWidth::I32);
      X86LegacySourceExecution Context(S, *M, O);
      uint64_t Budget = 1;
      ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
      EXPECT_EQ(Budget, 0u);
      EXPECT_EQ(Context.lastResult().DeliveredVector, Fault ? 14 : 1);
      EXPECT_EQ(Context.lastResult().InstructionCompleted, !Fault);
      EXPECT_EQ(M->controls().CR0, Fault ? 0x8005001fu : 0x80050015u);
      EXPECT_EQ(S.Debug->DR6, Fault ? 0xffff0ff0u
                              : TF  ? 0xffff4ff1u
                                    : 0xffff0ff1u);
      EXPECT_EQ(get(S.Core.GPR[4] + (Fault ? 4 : 0)),
                Fault ? 0x1000u : 0x1003u);
      EXPECT_EQ(get(0x9000), 0xdeadfff4u);
    }
}

TEST_P(X86LegacySourceTest, MachineStatusAccessControlOperandOverrideFaults) {
  for (uint8_t Opcode : {0xcau, 0xcbu}) {
    resetLegacyState({.PhysicalAddressBits = 32, .SMAP = true});
    Driver Run = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        compile({{0x1000, 32, {0x66, 0x0f, 1, Opcode}}}, Run));
    S.Core.CPL = 0;
    S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
    S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
    S.EFlags = 0x40ed7;
    X86LegacySourceExecution Context(S, *M, O);
    uint64_t Budget = 1;
    ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
    EXPECT_EQ(Budget, 0u);
    EXPECT_EQ(Context.lastResult().DeliveredVector, 6);
    EXPECT_FALSE(Context.lastResult().InstructionCompleted);
    EXPECT_EQ(get(S.Core.GPR[4]), 0x1000u);
    EXPECT_EQ(get(S.Core.GPR[4] + 8) & (1u << 18), 1u << 18);
  }
}

TEST_P(X86LegacySourceTest,
       SystemSelectorsPublishCachesForFollowingInstructions) {
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true}) {
      resetLegacyState();
      S.Core.CPL = 0;
      S.Core.Segments[1] = {0, UINT32_MAX, 8,
                            uint16_t(Code32 ? 0xc09b : 0x809b), false};
      S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
      O.SystemSelectors = X86LegacySystemSelectorProfile::ExclusiveTable;
      O.DescriptorQueries = X86LegacyDescriptorProfile::TableImage;
      S.Core.GPR[0] = S.Core.GPR[2] = 0x89abcdef;
      S.Core.GPR[3] = 7;
      S.Core.GPR[6] = 64;
      S.Core.GPR[7] = 59;
      put(0x7040, 0x0020820090000007ULL);
      put(0x7038, 0x0000890058000067ULL);
      put(0x9000, 0x0000f20000001234ULL);
      std::vector<SourceUnit> Units;
      uint32_t PC = 0x1000;
      for (const auto &[Form, Register] :
           {std::pair{2u, 6}, {0u, 0}, {3u, 7}, {1u, 2}}) {
        auto Bytes = test::legacySystemSelectorBytes(Form, Code32, Operand32,
                                                     true, Register);
        Units.push_back({PC, Code32 ? 32u : 16u, Bytes});
        PC += Bytes.size();
      }
      auto Query =
          test::legacyDescriptorBytes(1, Code32, Operand32, true, 1, 3);
      Units.push_back({PC, Code32 ? 32u : 16u, Query});
      Driver Run = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
      X86LegacySourceExecution Context(S, *M, O);
      uint64_t Budget = Units.size();
      ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
      EXPECT_EQ(Budget, 0u);
      EXPECT_EQ(S.EIP, PC + Query.size());
      EXPECT_EQ(S.Core.GPR[0], Operand32 ? 64u : 0x89ab0040u);
      EXPECT_EQ(S.Core.GPR[2], Operand32 ? 59u : 0x89ab003bu);
      EXPECT_EQ(S.Core.GPR[1], 0x1234u);
      EXPECT_EQ(S.TR.Base, 0x5800u);
      EXPECT_EQ(S.TR.Attributes, 0x8b);
      EXPECT_EQ(S.Core.LDTR.Base, 0x9000u);
      EXPECT_EQ(get(0x7038, GuestScalarWidth::I64), 0x00008b0058000067ULL);
    }
}

TEST_P(X86LegacySourceTest, SystemSelectorBindingRefusesBeforeAttemptOrFetch) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 0, 0xde}}}, Run));
  O.SystemSelectors.reset();
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  EXPECT_EQ(Run(&Context, &Budget), 256u);
  EXPECT_EQ(Budget, 1u);
  EXPECT_EQ(get(0x11004), 0x1007u);
}

TEST_P(X86LegacySourceTest, SystemSelectorBusyWriteFaultRepairAndRetry) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      compile({{0x1000, 32, {0x0f, 0, 0xde}},
               {0x1003, 32, {0x90}},
               {0x2000, 32, {0xc7, 0x05, 0x20, 0x10, 1, 0, 3, 0x80, 0, 0}},
               {0x200a, 32, {0x83, 0xc4, 4}},
               {0x200d, 32, {0xcf}}},
              Run));
  O.SystemSelectors = X86LegacySystemSelectorProfile::ExclusiveTable;
  S.Core.CPL = 0;
  S.Core.Segments[1] = {0, UINT32_MAX, 8, 0xc09b, false};
  S.Core.Segments[2] = {0, UINT32_MAX, 16, 0xc093, false};
  S.Core.GDTRLimit = 0x103f;
  S.Core.GPR[6] = 0x103b;
  put(0x8038, 0x0000890058000067ULL);
  put(0x11020, 0x8001, GuestScalarWidth::I32);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
  EXPECT_EQ(Context.lastResult().DeliveredVector, 14);
  EXPECT_EQ(S.TR.Selector, 40);
  EXPECT_EQ(S.CR2, 0x8038u);
  EXPECT_EQ(get(S.Core.GPR[4]), 3u);
  Budget = 5;
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(S.TR.Selector, 0x103b);
  EXPECT_EQ(S.EIP, 0x1004u);
  EXPECT_EQ(get(0x8038, GuestScalarWidth::I64), 0x00008b0058000067ULL);
}

TEST_P(X86LegacySourceTest, DescriptorQueriesWidthsLDTAndInvalidSelectors) {
  for (bool Code32 : {false, true})
    for (bool Operand32 : {false, true}) {
      std::vector<SourceUnit> Units;
      for (unsigned Form = 0; Form != 4; ++Form)
        Units.push_back({0x1000 + Form * 16, Code32 ? 32u : 16u,
                         test::legacyDescriptorBytes(Form, Code32, Operand32)});
      Driver Run = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
      for (unsigned Form = 0; Form != 4; ++Form)
        for (unsigned Case = 0; Case != 3; ++Case) {
          resetLegacyState();
          install(Units);
          O.DescriptorQueries = X86LegacyDescriptorProfile::TableImage;
          S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
          S.EIP = Units[Form].Address;
          S.EFlags = 0xed7;
          S.Core.GPR[0] = 0x12345678;
          S.Core.GPR[6] = Case == 0 ? 0xabcd003b : Case == 1 ? 7 : 0;
          S.Core.LDTR = {0x8000, 7, 48, 0x82, false};
          put(Case == 1 ? 0x8000 : 0x7038, 0x00da720000001234ULL);
          X86LegacySourceExecution Context(S, *M, O);
          uint64_t Budget = 1;
          ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
          const uint32_t Value = Form == 0 ? 0xda7200 : 0xa1234fff;
          EXPECT_EQ(S.Core.GPR[0], Case == 2 || Form >= 2 ? 0x12345678
                                   : Operand32 ? Value
                                               : 0x12340000 | uint16_t(Value));
          EXPECT_EQ(S.EFlags, Case == 2 ? 0xe97u : 0xed7u);
        }
    }
}

TEST_P(X86LegacySourceTest, DescriptorProfileRefusesBeforeAttemptOrFetch) {
  const std::vector<SourceUnit> Units{{0x1000, 32, {0x0f, 2, 0xc6}}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  O.DescriptorQueries.reset();
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  EXPECT_EQ(Run(&Context, &Budget), 256u);
  EXPECT_EQ(Budget, 1u);
  EXPECT_EQ(get(0x11004), 0x1007u);
}

TEST_P(X86LegacySourceTest,
       DescriptorPageFaultRepairAndRetryThroughGeneratedC) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      compile({{0x1000, 32, {0x0f, 3, 0xc6}},
               {0x1003, 32, {0x90}},
               {0x2000, 32, {0xc7, 0x05, 0x20, 0x10, 1, 0, 7, 0x80, 0, 0}},
               {0x200a, 32, {0x83, 0xc4, 4}},
               {0x200d, 32, {0xcf}}},
              Run));
  O.DescriptorQueries = X86LegacyDescriptorProfile::TableImage;
  S.Core.GDTRLimit = 0x103f;
  S.Core.GPR[6] = 0x103b;
  S.Core.GPR[0] = 0x12345678;
  put(0x8038, 0x00da720000001234ULL);
  put(0x11020, 0, GuestScalarWidth::I32);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
  EXPECT_EQ(Context.lastResult().DeliveredVector, 14);
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
  EXPECT_EQ(S.CR2, 0x8038u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0u);
  Budget = 5;
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(S.Core.GPR[0], 0xa1234fffu);
  EXPECT_EQ(S.EIP, 0x1004u);
  EXPECT_EQ(S.Core.CPL, 3);
}

TEST_P(X86LegacySourceTest, BMIValuesBothCodeWidthsAndMultiplyDestinations) {
  for (bool Code32 : {false, true}) {
    resetLegacyState();
    O.IntegerExtensions = {false, false, true, false, true};
    S.Core.Segments[1].Attributes = Code32 ? 0xc0fb : 0x80fb;
    std::vector<SourceUnit> Units;
    uint32_t Address = 0x1000;
    for (unsigned Form = 0; Form != test::LegacyBMIForms.size(); ++Form) {
      auto Bytes = test::legacyBMIBytes(Form, 0, 1, 6);
      Units.push_back({Address, Code32 ? 32u : 16u, Bytes});
      Address += Bytes.size();
    }
    Driver Run = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
    for (const auto &F : test::LegacyBMIForms) {
      SCOPED_TRACE(F.Name);
      S.Core.GPR[0] = 0x87654321;
      S.Core.GPR[1] = F.Control;
      S.Core.GPR[2] = 0xffffffff;
      S.Core.GPR[6] = F.Source;
      S.EFlags = 0xed7;
      O.BMIUndefinedFlags = F.UndefinedMask;
      X86LegacySourceExecution Context(S, *M, O);
      uint64_t Budget = 1;
      ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
      EXPECT_EQ(Budget, 0u);
      EXPECT_EQ(S.Core.GPR[0], F.Expected);
      EXPECT_EQ(S.Core.GPR[1], F.Opcode == 0xf6 ? 1 : F.Control);
      EXPECT_EQ(S.EFlags, F.UndefinedMask
                              ? 0x602 | F.DefinedFlags | F.UndefinedMask
                              : 0xed7);
    }
  }
}

TEST_P(X86LegacySourceTest, BMIFeaturesRefuseUnboundAndDeliverGuestUD) {
  const std::vector<SourceUnit> Units{
      {0x1000, 32, test::legacyBMIBytes(0, 0, 1, -1)},
      {0x1005, 32, test::legacyBMIBytes(6, 0, 1, -1)}};
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  for (unsigned Form : {0u, 6u}) {
    resetLegacyState();
    install(Units);
    S.EIP = Form ? 0x1005 : 0x1000;
    S.Core.GPR[3] = 0x9000;
    S.Core.Segments[3].Unusable = true;
    O.IntegerExtensions.reset();
    O.BMIUndefinedFlags.reset();
    uint64_t Budget = 1;
    X86LegacySourceExecution Unbound(S, *M, O);
    EXPECT_EQ(Run(&Unbound, &Budget), 256u);
    EXPECT_EQ(Budget, 1u);
    O.IntegerExtensions.emplace();
    X86LegacySourceExecution Bound(S, *M, O);
    ASSERT_EQ(Run(&Bound, &Budget), 1u) << Bound.lastError().str();
    EXPECT_EQ(Budget, 0u);
    EXPECT_EQ(Bound.lastResult().DeliveredVector, 6);
    EXPECT_EQ(get(S.Core.GPR[4]), Form ? 0x1005u : 0x1000u);
  }
}

TEST_P(X86LegacySourceTest, BMIMultiplyFaultRepairRetriesBothDestinations) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      compile({{0x1000, 32, test::legacyBMIBytes(6, 0, 1, -1)},
               {0x1005, 32, {0x90}},
               {0x2000, 32, {0xc7, 0x05, 0x24, 0x10, 1, 0, 7, 0x90, 0, 0}},
               {0x200a, 32, {0x83, 0xc4, 4}},
               {0x200d, 32, {0xcf}}},
              Run));
  O.IntegerExtensions = {false, false, false, false, true};
  S.Core.GPR[0] = 0x12345678;
  S.Core.GPR[1] = 0x87654321;
  S.Core.GPR[2] = 0xffffffff;
  S.Core.GPR[3] = 0x9000;
  put(0x9000, 0xffffffff, GuestScalarWidth::I32);
  put(0x11024, 0, GuestScalarWidth::I32);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
  EXPECT_EQ(S.Core.GPR[1], 0x87654321u);
  EXPECT_EQ(Context.lastResult().DeliveredVector, 14);
  Budget = 5;
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(S.Core.GPR[0], 0xfffffffeu);
  EXPECT_EQ(S.Core.GPR[1], 1u);
  EXPECT_EQ(S.EIP, 0x1006u);
  EXPECT_EQ(S.Core.CPL, 3);
}

TEST_P(X86LegacySourceTest, StoreChangingNextInstructionStopsBeforeItExecutes) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(
      {{0x1000, 32, {0xc6, 0x05, 7, 0x10, 0, 0, 0xcc}}, {0x1007, 32, {0x90}}},
      Run));
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 3;
  ASSERT_EQ(Run(&Context, &Budget), 3u) << Context.lastError().str();
  EXPECT_EQ(Budget, 2u);
  EXPECT_EQ(S.EIP, 0x1007u);
  EXPECT_EQ(get(0x1007, GuestScalarWidth::I8), 0xccu);
  EXPECT_FALSE(S.PendingEvent);
  EXPECT_FALSE(Context.lastResult().InstructionAttempted);
}

TEST_P(X86LegacySourceTest, RepPauseAndExternalEventPreserveElementProgress) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(
      {{0x1000, 32, {0xf3, 0xa4}}, {0x1002, 32, {0x90}}, {0x2000, 32, {0xcf}}},
      Run));
  O.Strings = X86LegacyStringProfile::ElementwiseDestinationFirst;
  S.Core.GPR[1] = 3;
  S.Core.GPR[6] = 0xb000;
  S.Core.GPR[7] = 0xc000;
  put(0xb000, 0x332211);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(Context.lastResult().Status,
            X86LegacyExecutionStatus::RepeatContinue);
  EXPECT_EQ(S.Core.GPR[1], 2u);
  EXPECT_EQ(get(0xc000), 0x11u);
  put(0xb000, 0xff, GuestScalarWidth::I8);
  ASSERT_EQ(Context.deliver({S.EIP, 0, 0, 65, X86LegacyEventKind::Interrupt}),
            1u);
  EXPECT_TRUE(Context.lastResult().EventAccepted);
  Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_EQ(S.Core.GPR[1], 2u);
  Budget = 3;
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(S.EIP, 0x1003u);
  EXPECT_EQ(get(0xc000), 0x332211u);
  EXPECT_EQ(S.Core.GPR[1], 0u);
}

TEST_P(X86LegacySourceTest,
       ChangedCodeBackingAndMissingProfilesDoNotSpendAttempts) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x89, 0xc0}}}, Run));
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 3;
  put(0x1001, 0xc1, GuestScalarWidth::I8);
  EXPECT_EQ(Run(&Context, &Budget), 3u);
  EXPECT_EQ(Budget, 3u);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_FALSE(Context.lastResult().InstructionAttempted);
  put(0x1001, 0xc0, GuestScalarWidth::I8);
  put(0x11004, 0x40007, GuestScalarWidth::I32);
  EXPECT_EQ(Run(&Context, &Budget), 4u);
  EXPECT_EQ(Budget, 3u);
  EXPECT_FALSE(S.PendingEvent);
  put(0x11004, 0x1007, GuestScalarWidth::I32);
  O.Scalars.reset();
  X86LegacySourceExecution Unbound(S, *M, O);
  EXPECT_EQ(Run(&Unbound, &Budget), 256u);
  EXPECT_EQ(Budget, 3u);
  EXPECT_NE(Unbound.lastError().find("ordinary-RAM"), llvm::StringRef::npos);
  EXPECT_EQ(get(0x11004), 0x1007u);
  O.Scalars = X86LegacyScalarProfile::OrderedRAM;
  X86LegacySourceExecution Restored(S, *M, O);
  EXPECT_EQ(neverd_x86_legacy_step_v1(&Restored, 0x1000, nullptr, 1), 256u);
  EXPECT_FALSE(Restored.lastResult().InstructionAttempted);
  EXPECT_EQ(neverd_x86_legacy_step_v1(&Restored, 0x100001000ULL, nullptr, 1),
            256u);
  EXPECT_FALSE(Restored.lastResult().InstructionAttempted);
  EXPECT_EQ(Run(&Restored, &Budget), 257u);
  EXPECT_EQ(Budget, 2u);
  EXPECT_TRUE(Restored.lastResult().InstructionCompleted);
}

TEST_P(X86LegacySourceTest, DeviceInputSurvivesPendingDebugEntryWithoutReplay) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      compile({{0x1000, 32, {0xec}}, {0x2000, 32, {0xcf}}}, Run));
  PagedPortTape Tape;
  O.PortIO = X86PortIOBinding{X86PortIOProfile::InputBeforeWrite,
                              PagedPortTape::transfer, &Tape};
  O.Debug = X86LegacyDebugProfile::InstructionStep;
  S.Debug.emplace();
  S.EFlags = 0x3302;
  S.Core.GPR[2] = 0x80;
  put(0x11018, 0x40007, GuestScalarWidth::I32);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 2;
  ASSERT_EQ(Run(&Context, &Budget), 4u) << Context.lastError().str();
  ASSERT_TRUE(S.PendingEvent);
  EXPECT_EQ(Tape.Transfers.size(), 1u);
  EXPECT_EQ(S.Core.GPR[0] & 0xff, 0x21u);
  EXPECT_TRUE(Context.lastResult().DeviceTransferCompleted);
  EXPECT_TRUE(Context.lastResult().InstructionCompleted);
  EXPECT_EQ(Budget, 1u);
  uint64_t Empty = 0;
  const auto Before = M->physicalMemory().snapshotMemoryRegions().front().Bytes;
  EXPECT_EQ(Run(&Context, &Empty),
            uint32_t(X86LegacySourceExit::BudgetExhausted));
  EXPECT_EQ(Empty, 0u);
  EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes, Before);
  EXPECT_EQ(Tape.Transfers.size(), 1u);
  ASSERT_TRUE(S.PendingEvent);
  EXPECT_EQ(Run(&Context, &Budget), 259u);
  EXPECT_EQ(Budget, 1u);
  EXPECT_EQ(Tape.Transfers.size(), 1u);
  put(0x11018, 0x6007, GuestScalarWidth::I32);
  ASSERT_EQ(Context.resume(), 1u) << Context.lastError().str();
  EXPECT_FALSE(S.PendingEvent);
  EXPECT_FALSE(Context.lastResult().InstructionAttempted);
  EXPECT_FALSE(Context.lastResult().EventAccepted);
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(S.EIP, 0x1001u);
  Budget = 2;
  EXPECT_EQ(Run(&Context, &Budget), 257u);
  EXPECT_EQ(Budget, 2u);
  EXPECT_EQ(Tape.Transfers.size(), 1u);
}

#if defined(__linux__) && defined(__x86_64__)
TEST_P(X86LegacySourceTest, BranchDebugMatchesOriginalProtectedInstructions) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  unsigned Comparisons = 0;
  for (bool Code16 : {false, true}) {
    const auto Cases = neverd::test::x86BranchDebugCases(Code16);
    std::vector<SourceUnit> Units;
    for (unsigned I = 0; I != Cases.size(); ++I) {
      unsigned Offset = 0;
      for (const auto Length : Cases[I].Lengths) {
        Units.push_back({0x1000 + I * 0x40 + Offset,
                         Code16 ? 16u : 32u,
                         {Cases[I].Code.begin() + Offset,
                          Cases[I].Code.begin() + Offset + Length}});
        Offset += Length;
      }
    }
    Driver Run = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
    neverd::test::X86BranchDebugOracle CPU;
    using Mode = neverd::test::X86BranchDebugOracle::Mode;
    ASSERT_NO_FATAL_FAILURE(
        CPU.initialize(Code16 ? Mode::Code16 : Mode::Code32));
    for (unsigned I = 0; I != Cases.size(); ++I)
      for (bool Branch : {false, true}) {
        SCOPED_TRACE(testing::Message() << Code16 << ':' << I << ':' << Branch);
        const auto &Case = Cases[I];
        neverd::test::X86BranchDebugOracle::Result Native;
        ASSERT_NO_FATAL_FAILURE(CPU.run(Case, Branch, Native));
        resetLegacyState();
        install(Units);
        S.EIP = 0x1000 + I * 0x40;
        const auto Start = S.EIP;
        if (Code16)
          S.Core.Segments[1].Attributes &= ~0x4000;
        S.EFlags = Case.Flags | 0x100;
        S.Core.GPR[0] = 0;
        S.Core.GPR[1] = Case.Count;
        O.Debug = X86LegacyDebugProfile::BranchWatchpoints;
        S.Debug.emplace();
        S.Debug->DebugCTL = Branch ? 2 : 0;
        put(0xd000, Start + 4,
            Code16 ? GuestScalarWidth::I16 : GuestScalarWidth::I32);
        X86LegacySourceExecution Context(S, *M, O);
        uint64_t Budget = Case.Lengths.size();
        ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
        ASSERT_EQ(Context.lastResult().DeliveredVector, 1);
        EXPECT_TRUE(Context.lastResult().InstructionCompleted);
        EXPECT_EQ(get(S.Core.GPR[4]), Start + Native.PC);
        EXPECT_EQ(S.Core.GPR[0], Native.Accumulator);
        EXPECT_EQ(S.Core.GPR[1], Native.Count);
        EXPECT_EQ(get(S.Core.GPR[4] + 8) & 0xcd5, Native.Flags & 0xcd5);
        EXPECT_EQ(int64_t(get(S.Core.GPR[4] + 12)) - 0xd000, Native.StackDelta);
        EXPECT_EQ(S.Debug->DR6, 0xffff4ff0u);
        EXPECT_EQ(S.Debug->DebugCTL, 0u);
        ++Comparisons;
      }
  }
  RecordProperty("branch_selection_comparisons", Comparisons);
}
#endif

TEST_P(X86LegacySourceTest,
       SDKAndCLIExportIdenticalSourceAndRejectMixedRoutes) {
  const std::vector<SourceUnit> Units{
      {0x1000, 32, {0xb8, 0x78, 0x56, 0x34, 0x12}}};
  const auto Input = tmpFile("input.elf"), Settings = tmpFile("request.json"),
             Output = tmpFile("output.c"), Report = tmpFile("manifest.json");
  // Independent minimal ELF32: one original executable PT_LOAD mapping.
  std::vector<uint8_t> Bytes(0x1005);
  const auto Put = [&](unsigned Offset, uint64_t Value, unsigned Count) {
    for (unsigned I = 0; I != Count; ++I)
      Bytes[Offset + I] = Value >> (I * 8);
  };
  Put(0, 0x10101464c457fULL, 7);
  Put(16, 2, 2);
  Put(18, 3, 2);
  Put(20, 1, 4);
  Put(24, 0x1000, 4);
  Put(28, 52, 4);
  Put(40, 52, 2);
  Put(42, 32, 2);
  Put(44, 1, 2);
  Put(52, 1, 4);
  Put(56, 0x1000, 4);
  Put(60, 0x1000, 4);
  Put(64, 0x1000, 4);
  Put(68, 5, 4);
  Put(72, 5, 4);
  Put(76, 5, 4);
  Put(80, 0x1000, 4);
  std::copy(Units.front().Bytes.begin(), Units.front().Bytes.end(),
            Bytes.begin() + 0x1000);
  std::ofstream File(Input, std::ios::binary);
  File.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  File.close();
  const auto Request = request(Units);
  std::ofstream(Settings) << Request;
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  auto Destroy = llvm::scope_exit([&] { neverd_session_destroy(Session); });
  const char *Manifest = reinterpret_cast<const char *>(uintptr_t{1});
  EXPECT_EQ(neverd_x86_legacy_source_v1(Session, Request.data(), Request.size(),
                                        &Manifest),
            nullptr);
  EXPECT_EQ(Manifest, nullptr);
  ASSERT_EQ(neverd_session_load(Session, Input.string().c_str()), 1);
  const auto *Source = neverd_x86_legacy_source_v1(Session, Request.data(),
                                                   Request.size(), &Manifest);
  ASSERT_NE(Source, nullptr) << neverd_last_error(Session);
  auto Free = llvm::scope_exit([&] {
    neverd_free_string(Source);
    neverd_free_string(Manifest);
  });
  auto Expected = llvm::cantFail(exportX86LegacySource(image(Units), Request));
  EXPECT_EQ(Source, Expected.Source);
  ASSERT_NE(Manifest, nullptr);
  EXPECT_EQ(Manifest, Expected.Manifest);
  // Clang separately qualifies the legacy object ABI. NeverC compiles the
  // portable dispatcher for the supported 64-bit host below.
  if (!(GetParam() & 2)) {
    const auto CrossSource = tmpFile("i386.c"), CrossObject = tmpFile("i386.o");
    std::ofstream(CrossSource) << Expected.Source;
    std::vector<std::string> CrossArgs{"--target=i386-unknown-linux-gnu",
                                       "-ffreestanding",
                                       "-std=c11",
                                       GetParam() & 1 ? "-O2" : "-O0",
                                       "-Wall",
                                       "-Wextra",
                                       "-Werror",
                                       "-c",
                                       CrossSource.string(),
                                       "-o",
                                       CrossObject.string()};
    const auto Cross = exec(Compiler, CrossArgs);
    ASSERT_TRUE(Cross.ok()) << Cross.err;
    const auto Object = readSourceArtifact(CrossObject);
    ASSERT_GE(Object.size(), 20u);
    EXPECT_EQ(Object.substr(0, 4), std::string("\x7f"
                                               "ELF",
                                               4));
    EXPECT_EQ(Object[4], 1);  // ELFCLASS32.
    EXPECT_EQ(Object[18], 3); // EM_386, not the executing host's x64 ABI.
    if (const auto *Artifacts =
            std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      fs::create_directories(Artifacts);
      const auto Name = "i386-route-" + std::to_string(GetParam());
      fs::copy_file(CrossSource, fs::path(Artifacts) / (Name + ".c"),
                    fs::copy_options::overwrite_existing);
      fs::copy_file(CrossObject, fs::path(Artifacts) / (Name + ".o"),
                    fs::copy_options::overwrite_existing);
    }
  }
  std::vector<std::string> Args{"decompile", Input.string(),
                                "--x86-legacy-source=" + Settings.string(),
                                "-o", Output.string()};
  EXPECT_FALSE(exec(ndBin(), Args).ok());
  EXPECT_FALSE(fs::exists(Output));
  Args.push_back("--continuation-report=" + Report.string());
  auto Ran = exec(ndBin(), Args);
  ASSERT_TRUE(Ran.ok()) << Ran.err;
  EXPECT_EQ(readSourceArtifact(Output), Expected.Source);
  EXPECT_EQ(readSourceArtifact(Report), Expected.Manifest);
  for (const auto &Extra :
       {std::string("--llvm"), "--x64-continuation=" + Settings.string(),
        std::string("--func=0x1000")}) {
    auto Bad = Args;
    Bad.push_back(Extra);
    EXPECT_FALSE(exec(ndBin(), Bad).ok());
    EXPECT_EQ(readSourceArtifact(Output), Expected.Source);
  }
  Args.back() = "--continuation-report=" + Output.string();
  EXPECT_FALSE(exec(ndBin(), Args).ok());
  EXPECT_EQ(readSourceArtifact(Output), Expected.Source);
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile(Units, Run));
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  EXPECT_EQ(Run(&Context, &Budget), 258u);
  EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
}

#include "X86LegacyControlSourceTests.inc"

INSTANTIATE_TEST_SUITE_P(Compilers, X86LegacySourceTest, testing::Range(0u, 4u),
                         [](const testing::TestParamInfo<unsigned> &I) {
                           return std::string(I.param & 2 ? "NeverC"
                                                          : "Clang") +
                                  (I.param & 1 ? "O2" : "O0");
                         });

} // namespace
