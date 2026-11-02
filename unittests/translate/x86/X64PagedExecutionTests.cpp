//===- X64PagedExecutionTests.cpp - Generated paged instructions ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile both source routes and execute their actual memory/fault bridges.
//===----------------------------------------------------------------------===//

#include "../../lift/NeverDLiftFixture.h"
#include "X64PagedNativeCPU.h"

#include "neverd/analysis/arch/x86_64/X64PagedExecution.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/X64ContinuationExport.h"

#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::translate;

namespace {

class X64PagedExecutionTest : public NeverDLiftTest {
protected:
  void SetUp() override {
#if !defined(__linux__) || !defined(__x86_64__)
    GTEST_SKIP() << "Generated paged execution currently qualifies Linux x64";
#else
    NeverDLiftTest::SetUp();
#endif
  }

  GuestState RAM;
  X64ContinuationStateV6 State;
  X86PagingFeatures Features{.PhysicalAddressBits = 48,
                             .PAE = true,
                             .LongMode = true,
                             .PAT = true,
                             .NX = true};
  X86PagingState Controls{.CR0 = 0x80010011,
                          .CR3 = 0x10000,
                          .CR4 = 0x20,
                          .EFER = 0xd00,
                          .RFlags = 2};
  std::unique_ptr<X86PagedMemory> Memory;
  using Run = uint64_t (*)(X64ContinuationStateV6 *, uint8_t *, uint64_t *);
  using Access = decltype(&neverd_x64_paged_access_v1);
  using Step = decltype(&neverd_x64_paged_step_v1);
  using Bind = void (*)(Access, Step);

  void reset(const std::vector<uint8_t> &Code) {
    State = {};
    Controls = {0x80010011, 0x10000, 0x20, 0xd00, 2};
    RAM = llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    RAM.Memory.push_back({0, MemoryPermission::Read | MemoryPermission::Write,
                          0, std::vector<uint8_t>(0x40000)});
    put(0x10000, 0x11007);
    put(0x11000, 0x12007);
    put(0x12000, 0x13007);
    for (unsigned I = 0; I != 64; ++I)
      put(0x13000 + I * 8, I * 4096 | 7);
    put(0x13100, 0x30007);
    put(0x13108, 0x31007);
    put(0x5008, 0x00af9b000000ffffULL);
    put(0x5010, 0x00cf93000000ffffULL);
    put(0x5018, 0x00affb000000ffffULL);
    put(0x5020, 0x00cff3000000ffffULL);
    put(0x8004, 0x1e000);
    put(0x8024, 0x1f000);
    for (unsigned V : {0u, 6u, 8u, 12u, 13u, 14u, 17u}) {
      put(0x6000 + V * 16,
          (0x4000 + V * 16) | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
      RAM.Memory.front().Bytes[0x4000 + V * 16] = 0xf4; // Original handler HLT.
    }
    std::copy(Code.begin(), Code.end(),
              RAM.Memory.front().Bytes.begin() + 0x1000);
    auto &Core = State.Core.Core.Core.Core;
    Core.PC = 0x1000;
    Core.RFlags = 2;
    Core.GPR[0] = 0x1122334455667788;
    Core.GPR[4] = 0x1e000;
    Core.GPR[5] = Core.GPR[7] = 0x20000;
    State.Core.CS = {0, UINT32_MAX, 8, 0xa09b};
    State.Core.SS = {0, UINT32_MAX, 16, 0xc093};
    State.GDTR = {0x5000, 0x3f};
    State.IDTR = {0x6000, 4095};
    State.TR = {0x8000, 0x67, 40, 0x8b};
    State.Unusable = 64;
    put(0x30000, 0x8877665544332211);
  }

  void put(uint64_t Address, uint64_t Value) {
    for (unsigned I = 0; I != 8; ++I)
      RAM.Memory.front().Bytes[Address + I] = Value >> (I * 8);
  }

  static BinaryImage image(std::vector<uint8_t> Code) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.Base = Image.Entry = 0x1000;
    Segment S;
    S.Name = ".text";
    S.VA = 0x1000;
    S.Size = S.FileSz = Code.size();
    S.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    S.Data = std::move(Code);
    Image.Segments.push_back(std::move(S));
    return Image;
  }

  void compile(const std::vector<uint8_t> &Code, bool LLVM,
               const std::string &Optimization, Run &Driver,
               const std::vector<uint64_t> &Addresses = {0x1000},
               const std::string &Compiler = NEVERD_TEST_CLANG) {
    llvm::json::Array PCs;
    for (auto PC : Addresses)
      PCs.push_back(PC);
    std::string Request;
    llvm::raw_string_ostream OS(Request);
    OS << llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"profile", "kernel-events-v6"},
                                   {"mxcsr_mask", "0xffff"},
                                   {"paging_address_bits", 48},
                                   {"paged_memory", true},
                                   {"instructions", std::move(PCs)}}));
    auto Export = exportX64Continuation(image(Code), Request, LLVM);
    ASSERT_TRUE(bool(Export)) << llvm::toString(Export.takeError());
    ASSERT_EQ(Export->Source.find("unknown value"), std::string::npos);
    const auto Parsed = llvm::cantFail(llvm::json::parse(Export->Manifest));
    ASSERT_EQ(Parsed.getAsObject()->getInteger("native_requests"), 0);
    const auto Name = (LLVM ? "llvm" : "high") + Optimization +
                      (Compiler == NEVERD_TEST_CLANG ? "-clang" : "-neverc");
    const auto Source = tmpFile(Name + ".c"), Library = tmpFile(Name + ".so");
    std::ofstream File(Source);
    File << Export->Source << R"C(
static uint64_t (*access_bridge)(void *, uint64_t, uint64_t, uint64_t);
static uint64_t (*step_bridge)(uint8_t *, uint8_t *, uint64_t, const uint8_t *,
    uint64_t, neverd_x64_paged_body_v1, neverd_x64_paged_body_v1, uint64_t, uint64_t);
void bind_paged(uint64_t (*access)(void *, uint64_t, uint64_t, uint64_t),
    uint64_t (*step)(uint8_t *, uint8_t *, uint64_t, const uint8_t *, uint64_t,
        neverd_x64_paged_body_v1, neverd_x64_paged_body_v1, uint64_t, uint64_t)) {
    access_bridge = access; step_bridge = step;
}
uint64_t neverd_x64_paged_access_v1(void *exchange, uint64_t address,
                                   uint64_t bytes, uint64_t flags) {
    return access_bridge(exchange, address, bytes, flags);
}
uint64_t neverd_x64_paged_step_v1(uint8_t *context, uint8_t *state,
    uint64_t address, const uint8_t *bytes, uint64_t size,
    neverd_x64_paged_body_v1 body, neverd_x64_paged_body_v1 validate,
    uint64_t mask, uint64_t bits) {
    return step_bridge(context, state, address, bytes, size, body, validate, mask, bits);
}
)C";
    File.close();
    if (const char *Artifacts =
            std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      fs::create_directories(Artifacts);
      fs::copy_file(Source, fs::path(Artifacts) / (Name + ".c"),
                    fs::copy_options::overwrite_existing);
      std::ofstream(fs::path(Artifacts) / (Name + ".json")) << Export->Manifest;
    }
    const auto Built =
        exec(Compiler,
             {"-std=c11", Optimization, "-fPIC", "-shared", "-Wl,-Bsymbolic",
              "-Werror=uninitialized", "-Werror=return-type", Source.string(),
              "-o", Library.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    std::string Error;
    auto DL = llvm::sys::DynamicLibrary::getPermanentLibrary(
        Library.string().c_str(), &Error);
    ASSERT_TRUE(DL.isValid()) << Error;
    const auto Binder =
        reinterpret_cast<Bind>(DL.getAddressOfSymbol("bind_paged"));
    ASSERT_NE(Binder, nullptr);
    Binder(&neverd_x64_paged_access_v1, &neverd_x64_paged_step_v1);
    Driver =
        reinterpret_cast<Run>(DL.getAddressOfSymbol("neverd_x64_run_paged_v6"));
    ASSERT_NE(Driver, nullptr);
  }
};

TEST_F(X64PagedExecutionTest, CompiledLoadUsesGuestRAMAndDeliversPageFault) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0x8b, 0x07};
  for (bool LLVM : {false, true})
    for (const std::string O : {"-O0", "-O2"}) {
      Run Driver = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver));
      for (bool Present : {false, true}) {
        reset(Code);
        if (!Present)
          put(0x13100, 0);
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecution Context(*Memory);
        uint64_t Budget = 1;
        const auto Exit =
            Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
        EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
        EXPECT_EQ(Budget, 0u);
        EXPECT_EQ(Exit, Present ? 9u : 7u);
        const auto &Core = State.Core.Core.Core.Core;
        EXPECT_EQ(Core.PC, Present ? 0x1003u : 0x40e0u);
        EXPECT_EQ(Core.GPR[0],
                  Present ? 0x8877665544332211ULL : 0x1122334455667788ULL);
        EXPECT_EQ(Context.lastResult().InstructionCompleted, Present);
        if (!Present) {
          EXPECT_EQ(State.Core.Core.CR2, 0x20000u);
          EXPECT_EQ(Core.GPR[4], 0x1dfd0u);
          EXPECT_EQ(Memory->physicalMemory()
                        .loadScalar(0x1dfd8, GuestScalarWidth::I64)
                        .Value,
                    0x1000u);
        }
      }
    }
}

TEST_F(X64PagedExecutionTest,
       ChangedCodeAndBackingFailureDoNotCommitTheInstruction) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0x8b, 0x07};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver));
    for (unsigned Failure = 0; Failure != 4; ++Failure) {
      reset(Code);
      if (Failure == 0)
        State.Core.Core.Core.Core.RFlags = 0;
      if (Failure == 1)
        RAM.Memory.front().Bytes[0x1000] = 0x90;
      if (Failure == 2)
        put(0x13100, 0x50007); // Missing owned physical backing.
      if (Failure == 3)
        Controls.CR4 |= uint64_t(1) << 32; // FRED requires another owner.
      const auto Before = State;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory);
      uint64_t Budget = 1;
      const auto Exit =
          Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
      EXPECT_EQ(Exit, Failure == 1 ? 20u : Failure == 2 ? 18u : 5u);
      EXPECT_EQ(Budget, 1u);
      EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
      EXPECT_FALSE(Context.lastResult().InstructionCompleted);
      EXPECT_FALSE(Context.lastResult().Exception);
      EXPECT_EQ(!Context.lastError().empty(), Failure == 3);
      if (Failure == 0 || Failure == 3)
        EXPECT_EQ(
            Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            RAM.Memory.front().Bytes);
    }
  }
}

TEST_F(X64PagedExecutionTest,
       WriteFaultCanRestartWithoutRepeatingACommittedEffect) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0x01, 0x07}; // add [rdi],rax
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver));
    reset(Code);
    put(0x13100, 0x30005);
    const auto Before = State;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecution Context(*Memory);
    uint64_t Budget = 1;
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              7u);
    ASSERT_TRUE(Context.lastResult().Exception);
    EXPECT_EQ(Context.lastResult().Exception->ErrorCode,
              3u); // RMW write intent.
    EXPECT_EQ(Memory->physicalMemory()
                  .loadScalar(0x30000, GuestScalarWidth::I64)
                  .Value,
              0x8877665544332211ULL);
    State = Before;
    ASSERT_EQ(Memory->physicalMemory()
                  .storeScalar(0x13100, GuestScalarWidth::I64, 0x30007)
                  .Status,
              GuestMemoryAccessStatus::Completed);
    Budget = 1;
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              9u);
    EXPECT_TRUE(Context.lastResult().InstructionCompleted);
    EXPECT_EQ(Memory->physicalMemory()
                  .loadScalar(0x30000, GuestScalarWidth::I64)
                  .Value,
              uint64_t(0x8877665544332211ULL + 0x1122334455667788ULL));
  }
}

TEST_F(X64PagedExecutionTest, SelfModificationStopsBeforeExecutingStaleSource) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0x89, 0x07, 0x90};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        compile(Code, LLVM, "-O2", Driver, {0x1000, 0x1003}));
    reset(Code);
    State.Core.Core.Core.Core.GPR[7] = 0x1003;
    State.Core.Core.Core.Core.GPR[0] = 0xcccccccccccccccc;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64PagedExecution Context(*Memory);
    uint64_t Budget = 2;
    EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              20u);
    EXPECT_EQ(Budget, 1u);
    EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x1003u);
    EXPECT_EQ(Memory->physicalMemory()
                  .loadScalar(0x1003, GuestScalarWidth::I64)
                  .Value,
              0xccccccccccccccccULL);
  }
}

TEST_F(X64PagedExecutionTest, NativeAndPartialCommitDomainsRemainExplicit) {
  X64ContinuationSourceOptions O;
  O.Profile = X64ContinuationProfile::KernelEventsNoFaultV6;
  O.MXCSRMask = 0xffff;
  O.PagingAddressBits = 48;
  O.PagedMemory = true;
  for (const auto &Code : std::vector<std::vector<uint8_t>>{
           {0xf3, 0x48, 0xa5}, // REP MOVSQ retains partial progress.
           {0xf0, 0x48, 0x01,
            0x07},             // Native LOCK is not a host-pointer operation.
           {0x0f, 0x20, 0xd8}, // Native CR3 needs its architectural transition.
           {0xfb}}) {
    auto Units = buildX64ContinuationSource(image(Code), {0x1000}, O);
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    EXPECT_TRUE(Units->front().RequiresNativeExecution);
  }
  O.NativePageTableAccess = true;
  auto Bad = buildX64ContinuationSource(image({0x90}), {0x1000}, O);
  ASSERT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest, GeneratedInstructionsMatchOriginalCPU) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    bool Target = false;
  };
  const std::vector<Case> Cases{
      {"load64", {0x48, 0x8b, 0x07}},
      {"load8", {0x8a, 0x07}},
      {"load16", {0x66, 0x8b, 0x07}},
      {"load32", {0x8b, 0x07}},
      {"store64", {0x48, 0x89, 0x07}},
      {"add-memory", {0x48, 0x01, 0x07}},
      {"xadd-memory", {0x48, 0x0f, 0xc1, 0x07}},
      {"div-memory", {0x48, 0xf7, 0x37}},
      {"idiv-memory", {0x48, 0xf7, 0x3f}},
      {"stack-reference", {0x48, 0x8b, 0x45, 0}},
      {"fs-base", {0x64, 0x48, 0x8b, 0x07}},
      {"address32", {0x67, 0x48, 0x8b, 0x07}},
      {"push-register", {0x50}},
      {"pop-register", {0x58}},
      {"push-memory", {0xff, 0x37}},
      {"pop-memory", {0x8f, 0x07}},
      {"call-register", {0xff, 0xd0}, true},
      {"call-memory", {0xff, 0x17}, true},
      {"return", {0xc3}, true},
      {"jump", {0xff, 0xe0}, true},
      {"address-only", {0x48, 0x8d, 0x07}},
      {"undefined", {0x0f, 0x0b}},
      {"add-register", {0x48, 0x01, 0xd8}},
      {"conditional-memory", {0x48, 0x0f, 0x44, 0x07}},
      {"store8", {0x88, 0x07}},
      {"store-high-byte", {0x88, 0x27}},
      {"store16", {0x66, 0x89, 0x07}},
      {"store32", {0x89, 0x07}},
      {"add-memory8", {0x00, 0x07}},
      {"add-memory16", {0x66, 0x01, 0x07}},
      {"add-memory32", {0x01, 0x07}},
      {"div-memory8", {0xf6, 0x37}},
      {"idiv-memory8", {0xf6, 0x3f}},
      {"div-memory16", {0x66, 0xf7, 0x37}},
      {"idiv-memory16", {0x66, 0xf7, 0x3f}},
      {"div-memory32", {0xf7, 0x37}},
      {"idiv-memory32", {0xf7, 0x3f}},
      {"split-fetch", {0x48, 0x8b, 0x07}}};
  std::vector<uint8_t> Code(0x1001, 0x90);
  std::vector<uint64_t> PCs;
  for (unsigned I = 0; I != Cases.size(); ++I) {
    const uint64_t PC = I + 1 == Cases.size() ? 0x1ffe : 0x1000 + I * 32;
    PCs.push_back(PC);
    std::copy(Cases[I].Bytes.begin(), Cases[I].Bytes.end(),
              Code.begin() + PC - 0x1000);
  }
  X64ContinuationSourceOptions Options;
  Options.Profile = X64ContinuationProfile::KernelEventsNoFaultV6;
  Options.MXCSRMask = 0xffff;
  Options.PagingAddressBits = 48;
  Options.PagedMemory = true;
  auto Units = buildX64ContinuationSource(image(Code), PCs, Options);
  ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
  for (const auto &Unit : *Units)
    ASSERT_FALSE(Unit.RequiresNativeExecution)
        << Unit.Function.Name << ": " << Unit.Diagnostic;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.push_back(NeverC);
  unsigned Comparisons = 0, FaultComparisons = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver, PCs, Compiler));
        for (unsigned I = 0; I != Cases.size(); ++I)
          for (unsigned Variant = 0; Variant != 10; ++Variant) {
            SCOPED_TRACE(testing::Message()
                         << Compiler << ' ' << LLVM << ' ' << O << ' '
                         << Cases[I].Name << " variant=" << Variant);
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = PCs[I];
            Core.GPR[2] = 0;
            Core.GPR[3] = 0x7788;
            Core.RFlags = Variant & 1 ? 0x206 : 0x246;
            if (Variant == 1)
              put(0x13100, 0);
            if (Variant == 2)
              put(0x13100, 0x30005);
            if (Variant == 3) {
              Core.GPR[7] = Core.GPR[5] = 0x20fff;
              put(0x13108, 0);
            }
            if (Variant == 4) {
              Core.GPR[7] = Core.GPR[5] = 0x20001;
              Core.RFlags |= 0x40000;
              Controls.CR0 |= 0x40000;
              State.Core.CS.Selector = 27;
              State.Core.CS.Attributes = 0xa0fb;
              State.Core.SS.Selector = 35;
              State.Core.SS.Attributes = 0xc0f3;
              Core.GPR[4] = 0x1c000;
            }
            if (Variant == 5)
              Core.GPR[7] = Core.GPR[5] = 0x800000000000;
            if (Variant == 6) {
              put(0x13000 + 0x1d * 8, 0);
              for (unsigned V : {0u, 6u, 8u, 12u, 13u, 14u, 17u})
                put(0x6000 + V * 16, (0x4000 + V * 16) | (uint64_t(8) << 16) |
                                         (uint64_t(1) << 32) |
                                         (uint64_t(0x8e) << 40));
            }
            if (Variant == 7)
              put(0x30000, 0);
            if (Variant == 8)
              put(0x13000 + (PCs[I] / 4096) * 8, 0);
            if (Variant == 9)
              put(0x13010, 0);
            if (Cases[I].Target) {
              const auto Target = Variant == 5 || Variant == 6
                                      ? uint64_t(0x800000000000)
                                  : Variant == 7 ? 0u
                                                 : 0x1800u;
              Core.GPR[0] = Target;
              put(0x30000, Target);
              put(Core.GPR[4], Target);
            } else
              put(Core.GPR[4], 0x123456789abcdef0);
            if (I == 10)
              Core.FSBase = 0x100;
            if (I == 11)
              Core.GPR[7] |= 0x1234000000000000;
            X64ContinuationStateV6 Native;
            bool Fault = false;
            ASSERT_NO_FATAL_FAILURE(
                CPU.execute(RAM.Memory.front().Bytes, State, Controls.CR0,
                            Cases[I].Bytes.size(), Native, Fault));
            // Bind only architecturally arbitrary outputs, using this original
            // instruction run. Undefined flags do not become fixed semantics.
            for (unsigned E = 0; E != (*Units)[I].UndefinedEffects.size();
                 ++E) {
              const auto Offset = (*Units)[I].UndefinedEffects[E].Output.Offset;
              const std::pair<uint64_t, unsigned> Flags[] = {
                  {x86reg::CF, 0}, {x86reg::PF, 2}, {x86reg::AF, 4},
                  {x86reg::ZF, 6}, {x86reg::SF, 7}, {x86reg::OF, 11}};
              bool Found = false;
              for (auto [Reg, Bit] : Flags)
                if (Reg == Offset) {
                  Core.Undefined[E] =
                      (Native.Core.Core.Core.Core.RFlags >> Bit) & 1;
                  Found = true;
                }
              ASSERT_TRUE(Found) << "unbound arbitrary output " << Offset;
            }
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(*Memory);
            uint64_t Budget = 1;
            const auto Exit =
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
            ASSERT_TRUE(Context.lastError().empty()) << Context.lastError();
            ASSERT_NE(Exit, 5u);
            EXPECT_EQ(Budget, 0u);
            EXPECT_EQ(Exit == 7, Fault);
            EXPECT_EQ(Context.lastResult().InstructionCompleted, !Fault);
            const auto &Expected = Native.Core.Core.Core.Core;
            EXPECT_EQ(Core.PC, Expected.PC);
            EXPECT_EQ(Core.RFlags, Expected.RFlags);
            for (unsigned R = 0; R != 16; ++R)
              EXPECT_EQ(Core.GPR[R], Expected.GPR[R]) << "GPR " << R;
            EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
            EXPECT_EQ(State.Core.CS.Selector, Native.Core.CS.Selector);
            EXPECT_EQ(State.Core.SS.Selector, Native.Core.SS.Selector);
            const auto ActualRAM =
                Memory->physicalMemory().snapshotMemoryRegions();
            // Fault-time paging A/D updates are CPU-dependent. All operand,
            // descriptor and frame bytes outside those table pages are exact.
            EXPECT_EQ(
                std::memcmp(ActualRAM.front().Bytes.data(), CPU.RAM, 0x10000),
                0);
            EXPECT_EQ(std::memcmp(ActualRAM.front().Bytes.data() + 0x14000,
                                  CPU.RAM + 0x14000,
                                  X64PagedNativeCPU::Size - 0x14000),
                      0);
            ++Comparisons;
            FaultComparisons += Fault;
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("fault_cpu_comparisons", FaultComparisons);
}
#endif
} // namespace
