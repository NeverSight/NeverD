//===- X64PagedExecutionTests.cpp - Generated paged instructions ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile both source routes and execute their actual memory/fault bridges.
//===----------------------------------------------------------------------===//

#include "../../HostCpu.h"
#include "../../lift/NeverDLiftFixture.h"
#include "X64PagedNativeCPU.h"
#include "X86BranchDebugOracle.h"
#include "X86LegacyMachineStatusTestEncoding.h"
#include "X86NativeDebugStatus.h"
#include "X86PortDevice.h"

#include "neverd/analysis/arch/x86_64/X64PagedExecution.h"
#include "neverd/analysis/arch/x86_64/X86ModeExecution.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X86UndefinedOpcodeRules.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/X64ContinuationExport.h"
#include "neverd/translate/X86/X86WatchpointObservation.h"

#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
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
  using Validate = decltype(&neverd_x64_paged_validate_v1);
  using Event = decltype(&neverd_x64_paged_event_v1);
  using Progress = decltype(&neverd_x64_paged_progress_v1);
  using DebugStep = decltype(&neverd_x64_paged_debug_step_v2);
  using DebugValidate = decltype(&neverd_x64_paged_debug_validate_v2);
  using Bind = void (*)(Access, Step, Validate, Event, Progress, DebugStep,
                        DebugValidate, Access, Progress);
  struct Shadows {
    uint8_t STIBlocked = 0, SSBlocked = 0, NMIBlockedBySTI = 0, Reserved[5]{};
  };
  struct Pending {
    uint8_t Vector = 65, Kind = 2;
  };
  struct Producer {
    Pending Candidate;
    int Ready = 1;
    unsigned Polls = 0, Acks = 0;
    static int peek(const X64ContinuationStateV6 *, const Shadows *, Pending *P,
                    void *Context) {
      auto &Self = *static_cast<Producer *>(Context);
      ++Self.Polls;
      *P = Self.Candidate;
      return Self.Ready;
    }
    static void accepted(const Pending *P, void *Context) {
      auto &Self = *static_cast<Producer *>(Context);
      EXPECT_EQ(P->Vector, Self.Candidate.Vector);
      EXPECT_EQ(P->Kind, Self.Candidate.Kind);
      ++Self.Acks;
    }
  };
  using Schedule = uint64_t (*)(X64ContinuationStateV6 *, uint8_t *, uint64_t *,
                                Shadows *, uint64_t *,
                                decltype(&Producer::peek),
                                decltype(&Producer::accepted), void *);
  unsigned Compilation = 0;

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
    for (unsigned V = 0; V != 256; ++V) {
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

  static BinaryImage image(std::vector<uint8_t> Code, uint64_t Base = 0x1000) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.Base = Image.Entry = Base;
    Segment S;
    S.Name = ".text";
    S.VA = Base;
    S.Size = S.FileSz = Code.size();
    S.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    S.Data = std::move(Code);
    Image.Segments.push_back(std::move(S));
    return Image;
  }

  void
  compile(const std::vector<uint8_t> &Code, bool LLVM,
          const std::string &Optimization, Run &Driver,
          const std::vector<uint64_t> &Addresses = {0x1000},
          const std::string &Compiler = NEVERD_TEST_CLANG,
          uint64_t Base = 0x1000, Schedule *Scheduled = nullptr,
          std::optional<X64NullSegmentBasePolicy> NullBasePolicy = std::nullopt,
          bool ExpectNative = false, bool Debug = false) {
    llvm::json::Array PCs;
    for (auto PC : Addresses)
      PCs.push_back(PC);
    std::string Request;
    llvm::raw_string_ostream OS(Request);
    llvm::json::Object Settings{{"profile", "kernel-events-v6"},
                                {"mxcsr_mask", "0xffff"},
                                {"paging_address_bits", 48},
                                {"paged_memory", true},
                                {"paged_debug", Debug},
                                {"scheduled_events", Scheduled != nullptr},
                                {"follow_internal_calls", Scheduled != nullptr},
                                {"instructions", std::move(PCs)}};
    if (NullBasePolicy)
      Settings["null_segment_base_policy"] =
          *NullBasePolicy == X64NullSegmentBasePolicy::Clear ? "clear"
                                                             : "preserve";
    OS << llvm::formatv("{0}", llvm::json::Value(std::move(Settings)));
    auto Export = exportX64Continuation(image(Code, Base), Request, LLVM);
    ASSERT_TRUE(bool(Export)) << llvm::toString(Export.takeError());
    ASSERT_EQ(Export->Source.find("unknown value"), std::string::npos);
    ASSERT_EQ(Export->Source.find("bad source call"), std::string::npos);
    if (Debug) {
      ASSERT_EQ(Export->Source.find("neverd_x64_paged_access_v1"),
                std::string::npos);
    }
    const auto Parsed = llvm::cantFail(llvm::json::parse(Export->Manifest));
    ASSERT_EQ(Parsed.getAsObject()->getInteger("native_requests"),
              unsigned(ExpectNative))
        << Export->Manifest;
    auto Name = (LLVM ? "llvm" : "high") + Optimization +
                (Compiler == NEVERD_TEST_CLANG ? "-clang" : "-neverc") + "-" +
                testing::UnitTest::GetInstance()->current_test_info()->name() +
                "-" + std::to_string(Compilation++);
    if (Base != 0x1000)
      Name += "-at-" + std::to_string(Addresses.front());
    llvm::sys::DynamicLibrary DL;
    ASSERT_NO_FATAL_FAILURE(
        compileProgram(*Export, Name, Compiler, Optimization, DL));
    if (Scheduled) {
      *Scheduled = reinterpret_cast<Schedule>(
          DL.getAddressOfSymbol("neverd_x64_continue_paged_events_v6"));
      ASSERT_NE(*Scheduled, nullptr);
    }
    Driver =
        reinterpret_cast<Run>(DL.getAddressOfSymbol("neverd_x64_run_paged_v6"));
    ASSERT_NE(Driver, nullptr);
  }

  void compileProgram(const X64ContinuationExport &Export,
                      const std::string &Name, const std::string &Compiler,
                      const std::string &Optimization,
                      llvm::sys::DynamicLibrary &DL,
                      llvm::StringRef ExtraSource = {}) {
    // Permanent library handles outlive each test fixture. Different suites
    // may use the same test name and compilation index in this process.
    const auto QualifiedName = std::string(testing::UnitTest::GetInstance()
                                               ->current_test_info()
                                               ->test_suite_name()) +
                               "-" + Name;
    const auto Source = tmpFile(QualifiedName + ".c"),
               Library = tmpFile(QualifiedName + ".so");
    std::ofstream File(Source);
    File << Export.Source << R"C(
static uint64_t (*access_bridge)(void *, uint64_t, uint64_t, uint64_t);
static uint64_t (*debug_access_bridge)(void *, uint64_t, uint64_t, uint64_t);
static uint64_t (*step_bridge)(uint8_t *, uint8_t *, uint64_t, const uint8_t *,
    uint64_t, neverd_x64_paged_body_v1, neverd_x64_paged_body_v1, uint64_t, uint64_t);
static uint64_t (*validate_bridge)(uint8_t *, uint8_t *, uint64_t,
    neverd_x64_paged_body_v1, uint64_t);
static uint64_t (*event_bridge)(uint8_t *, uint8_t *, const uint8_t *, uint64_t);
static uint64_t (*progress_bridge)(uint8_t *);
static uint64_t (*pending_bridge)(uint8_t *);
static uint64_t (*debug_step_bridge)(uint8_t *, uint8_t *, uint64_t, const uint8_t *,
    uint64_t, neverd_x64_paged_body_v1, neverd_x64_paged_body_v1, uint64_t, uint64_t, uint64_t);
static uint64_t (*debug_validate_bridge)(uint8_t *, uint8_t *, uint64_t,
    neverd_x64_paged_body_v1, uint64_t);
void bind_paged(uint64_t (*access)(void *, uint64_t, uint64_t, uint64_t),
    uint64_t (*step)(uint8_t *, uint8_t *, uint64_t, const uint8_t *, uint64_t,
        neverd_x64_paged_body_v1, neverd_x64_paged_body_v1, uint64_t, uint64_t),
    uint64_t (*validate)(uint8_t *, uint8_t *, uint64_t,
        neverd_x64_paged_body_v1, uint64_t),
    uint64_t (*event)(uint8_t *, uint8_t *, const uint8_t *, uint64_t),
    uint64_t (*progress)(uint8_t *),
    uint64_t (*debug_step)(uint8_t *, uint8_t *, uint64_t, const uint8_t *, uint64_t,
        neverd_x64_paged_body_v1, neverd_x64_paged_body_v1, uint64_t, uint64_t, uint64_t),
    uint64_t (*debug_validate)(uint8_t *, uint8_t *, uint64_t,
        neverd_x64_paged_body_v1, uint64_t),
    uint64_t (*debug_access)(void *, uint64_t, uint64_t, uint64_t),
    uint64_t (*pending)(uint8_t *)) {
    access_bridge = access; step_bridge = step;
    validate_bridge = validate; event_bridge = event; progress_bridge = progress;
    debug_step_bridge = debug_step; debug_validate_bridge = debug_validate;
    debug_access_bridge = debug_access; pending_bridge = pending;
}
uint64_t neverd_x64_paged_access_v1(void *exchange, uint64_t address,
                                   uint64_t bytes, uint64_t flags) {
    return access_bridge(exchange, address, bytes, flags);
}
uint64_t neverd_x64_paged_debug_access_v1(void *exchange, uint64_t address,
    uint64_t bytes, uint64_t flags) {
    return debug_access_bridge(exchange, address, bytes, flags);
}
uint64_t neverd_x64_paged_step_v1(uint8_t *context, uint8_t *state,
    uint64_t address, const uint8_t *bytes, uint64_t size,
    neverd_x64_paged_body_v1 body, neverd_x64_paged_body_v1 validate,
    uint64_t mask, uint64_t bits) {
    return step_bridge(context, state, address, bytes, size, body, validate, mask, bits);
}
uint64_t neverd_x64_paged_validate_v1(uint8_t *context, uint8_t *state,
    uint64_t address, neverd_x64_paged_body_v1 validate, uint64_t bits) {
    return validate_bridge(context, state, address, validate, bits);
}
uint64_t neverd_x64_paged_event_v1(uint8_t *context, uint8_t *state,
    const uint8_t *event, uint64_t mask) {
    return event_bridge(context, state, event, mask);
}
uint64_t neverd_x64_paged_progress_v1(uint8_t *context) {
    return progress_bridge(context);
}
uint64_t neverd_x64_paged_pending_v1(uint8_t *context) {
    return pending_bridge(context);
}
uint64_t neverd_x64_paged_debug_step_v2(uint8_t *context, uint8_t *state,
    uint64_t address, const uint8_t *bytes, uint64_t size,
    neverd_x64_paged_body_v1 body, neverd_x64_paged_body_v1 validate,
    uint64_t mask, uint64_t bits, uint64_t ss_blocked) {
    return debug_step_bridge(context, state, address, bytes, size, body, validate,
                             mask, bits, ss_blocked);
}
uint64_t neverd_x64_paged_debug_validate_v2(uint8_t *context, uint8_t *state,
    uint64_t address, neverd_x64_paged_body_v1 validate, uint64_t bits) {
    return debug_validate_bridge(context, state, address, validate, bits);
}
)C";
    File << ExtraSource.str();
    File.close();
    if (const char *Artifacts =
            std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
      fs::create_directories(Artifacts);
      fs::copy_file(Source, fs::path(Artifacts) / (QualifiedName + ".c"),
                    fs::copy_options::overwrite_existing);
      std::ofstream(fs::path(Artifacts) / (QualifiedName + ".json"))
          << Export.Manifest;
    }
    const auto Built =
        exec(Compiler,
             {"-std=c11", Optimization, "-fPIC", "-shared", "-Wl,-Bsymbolic",
              "-Werror=uninitialized", "-Werror=return-type", Source.string(),
              "-o", Library.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    std::string Error;
    DL = llvm::sys::DynamicLibrary::getPermanentLibrary(
        Library.string().c_str(), &Error);
    ASSERT_TRUE(DL.isValid()) << Error;
    const auto Binder =
        reinterpret_cast<Bind>(DL.getAddressOfSymbol("bind_paged"));
    ASSERT_NE(Binder, nullptr);
    Binder(&neverd_x64_paged_access_v1, &neverd_x64_paged_step_v1,
           &neverd_x64_paged_validate_v1, &neverd_x64_paged_event_v1,
           &neverd_x64_paged_progress_v1, &neverd_x64_paged_debug_step_v2,
           &neverd_x64_paged_debug_validate_v2,
           &neverd_x64_paged_debug_access_v1, &neverd_x64_paged_pending_v1);
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

TEST_F(X64PagedExecutionTest, SseCrossPageOperandsPublishTogether) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool Integer : {false, true})
    for (bool Store : {false, true}) {
      std::vector<uint8_t> Code;
      if (Integer)
        Code.push_back(0xf3);
      Code.insert(Code.end(), {0x0f,
                               uint8_t(Integer ? (Store ? 0x7f : 0x6f)
                                               : (Store ? 0x11 : 0x10)),
                               0x07}); // xmm0 and [rdi].
      for (bool LLVM : {false, true}) {
        Run Driver = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver));
        for (bool Present : {false, true}) {
          reset(Code);
          Controls.CR4 |= 0x200;
          auto &Core = State.Core.Core.Core.Core;
          Core.GPR[7] = 0x20ff8;
          for (unsigned I = 0; I != 64; ++I)
            Core.ZMM[0][I] = 0x40 + I;
          put(0x30ff8, 0x8877665544332211ULL);
          put(0x31000, 0xfedcba9876543210ULL);
          if (!Present)
            put(0x13108, 0);
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Context(*Memory);
          uint64_t Budget = 1;
          const auto Exit =
              Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
          EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
          EXPECT_EQ(Exit, Present ? 9u : 7u);
          EXPECT_EQ(Budget, 0u);
          EXPECT_EQ(Context.lastResult().InstructionCompleted, Present);
          EXPECT_EQ(Memory->physicalMemory()
                        .loadScalar(0x30ff8, GuestScalarWidth::I64)
                        .Value,
                    Store && Present ? 0x4746454443424140ULL
                                     : 0x8877665544332211ULL);
          EXPECT_EQ(Memory->physicalMemory()
                        .loadScalar(0x31000, GuestScalarWidth::I64)
                        .Value,
                    Store && Present ? 0x4f4e4d4c4b4a4948ULL
                                     : 0xfedcba9876543210ULL);
          for (unsigned I = 0; I != 64; ++I) {
            const uint64_t Word =
                I < 8 ? 0x8877665544332211ULL : 0xfedcba9876543210ULL;
            EXPECT_EQ(Core.ZMM[0][I], !Store && Present && I < 16
                                          ? uint8_t(Word >> ((I % 8) * 8))
                                          : 0x40 + I);
          }
          if (!Present) {
            ASSERT_TRUE(Context.lastResult().Exception);
            EXPECT_EQ(Context.lastResult().Exception->Vector, 14);
            EXPECT_EQ(State.Core.Core.CR2, 0x21000u);
          }
        }
      }
    }
}

TEST_F(X64PagedExecutionTest, SseAvailabilityPrecedesOperandFaults) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x0f, 0x10, 0x07};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver));
    for (unsigned Bits = 0; Bits != 8; ++Bits) {
      reset(Code);
      Controls.CR0 |= (Bits & 3) << 2; // EM and TS.
      if (Bits & 4)
        Controls.CR4 |= 0x200;
      put(0x13100, 0); // Operand is absent even when SSE is unavailable.
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory);
      uint64_t Budget = 1;
      EXPECT_EQ(Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                7u);
      EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
      ASSERT_TRUE(Context.lastResult().Exception);
      const unsigned Vector = !(Bits & 4) || (Bits & 1) ? 6
                              : (Bits & 2)              ? 7
                                                        : 14;
      EXPECT_EQ(Context.lastResult().Exception->Vector, Vector);
      EXPECT_EQ(State.Core.Core.CR2, Vector == 14 ? 0x20000u : 0u);
      EXPECT_EQ(Budget, 0u);
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
           {0xf2, 0x6c}, // Noncanonical port repeat remains a native boundary.
           {0xf0, 0x48, 0x01,
            0x07},             // Native LOCK is not a host-pointer operation.
           {0x0f, 0x20, 0xd8}, // Native CR3 needs its architectural transition.
           {0xfb},
           {0x66, 0xfb}}) {
    auto Units = buildX64ContinuationSource(image(Code), {0x1000}, O);
    ASSERT_TRUE(bool(Units)) << llvm::toString(Units.takeError());
    EXPECT_TRUE(Units->front().RequiresNativeExecution);
  }
  std::vector<uint8_t> Overlong(X86PagedCode64UD0QualifiedPrefixLimit + 1,
                                0x66);
  Overlong.insert(Overlong.end(), {0x0f, 0xff});
  auto Unsupported = buildX64ContinuationSource(image(Overlong), {0x1000}, O);
  if (Unsupported) {
    ASSERT_FALSE(Unsupported->empty());
    EXPECT_TRUE(Unsupported->front().RequiresNativeExecution);
  } else {
    llvm::consumeError(Unsupported.takeError());
  }
  auto Repeated =
      buildX64ContinuationSource(image({0xf3, 0x67, 0x48, 0xa5}), {0x1000}, O);
  ASSERT_TRUE(bool(Repeated)) << llvm::toString(Repeated.takeError());
  EXPECT_FALSE(Repeated->front().RequiresNativeExecution);
  auto Ordinary = O;
  Ordinary.PagedMemory = false;
  auto Unbound = buildX64ContinuationSource(image({0xf3, 0x67, 0x48, 0xa5}),
                                            {0x1000}, Ordinary);
  ASSERT_TRUE(bool(Unbound)) << llvm::toString(Unbound.takeError());
  EXPECT_TRUE(Unbound->front().RequiresNativeExecution);
  O.NativePageTableAccess = true;
  auto Bad = buildX64ContinuationSource(image({0x90}), {0x1000}, O);
  ASSERT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
}

TEST(X64PagedUD0PrefixTest, QualifiedPrefixesMatchTheDecoder) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X64));
  constexpr std::array<uint8_t, 26> Prefixes{
      0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf2,
      0xf3, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
      0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
  unsigned Checked = 0;
  for (unsigned PrefixCount = 0; PrefixCount <= 2; ++PrefixCount) {
    const unsigned FirstCount = PrefixCount ? Prefixes.size() : 1;
    const unsigned SecondCount = PrefixCount == 2 ? Prefixes.size() : 1;
    for (unsigned First = 0; First != FirstCount; ++First)
      for (unsigned Second = 0; Second != SecondCount; ++Second) {
        std::vector<uint8_t> Code;
        if (PrefixCount)
          Code.push_back(Prefixes[First]);
        if (PrefixCount == 2)
          Code.push_back(Prefixes[Second]);
        Code.insert(Code.end(), {0x0f, 0xff, 0xc0});
        SCOPED_TRACE(testing::Message()
                     << "first=" << First << " second=" << Second);
        const auto Offset =
            x86PagedCode64UD0OpcodeOffset(Code.data(), Code.size() - 1);
        ASSERT_TRUE(Offset);
        EXPECT_EQ(*Offset, PrefixCount);
        const auto WithAddressTail =
            x86PagedCode64UD0OpcodeOffset(Code.data(), Code.size());
        ASSERT_TRUE(WithAddressTail);
        EXPECT_EQ(*WithAddressTail, PrefixCount);
        DecodedInsn Instruction;
        ASSERT_EQ(Decode.decodeOneForLift(Code.data(), Code.size(), 0x1000,
                                          Instruction),
                  PrefixCount + X86PagedCode64UD0OpcodeBytes);
        EXPECT_EQ(Instruction.Id, X86_INS_UD0);
        ++Checked;
      }
  }
  EXPECT_EQ(Checked, 703u);
  for (const auto &Code : {std::vector<uint8_t>{0xf0, 0x0f, 0xff},
                           std::vector<uint8_t>{0xf0, 0x66, 0x0f, 0xff}})
    EXPECT_FALSE(x86PagedCode64UD0OpcodeOffset(Code.data(), Code.size()));
}

TEST(X64PagedUD0PrefixTest, ExtendedPrefixesAndLengthBoundaryMatchTheDecoder) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X64));
  constexpr std::array<uint8_t, 26> RepeatedPrefixes{
      0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf2,
      0xf3, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
      0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
  constexpr std::array<
      std::array<uint8_t, X86PagedCode64UD0QualifiedPrefixLimit>, 5>
      Patterns{{
          {{0x66, 0x67, 0x66, 0x67, 0x66, 0x67, 0x66, 0x67, 0x66, 0x67, 0x66,
            0x67, 0x66}},
          {{0x66, 0x67, 0xf2, 0x64, 0x4f, 0x2e, 0xf3, 0x40, 0x65, 0x3e, 0x48,
            0x36, 0x26}},
          {{0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
            0x4b, 0x4c}},
          {{0xf2, 0xf3, 0xf2, 0xf3, 0xf2, 0xf3, 0xf2, 0xf3, 0xf2, 0xf3, 0xf2,
            0xf3, 0xf2}},
          {{0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x26, 0x2e, 0x36, 0x3e, 0x64,
            0x65, 0x26}},
      }};
  EXPECT_EQ(X86PagedCode64UD0QualifiedPrefixLimit, 13u);
  unsigned Checked = 0;
  const auto Check = [&](const std::vector<uint8_t> &Prefix) {
    std::vector<uint8_t> Code = Prefix;
    Code.insert(Code.end(), {0x0f, 0xff, 0xc0});
    SCOPED_TRACE(testing::Message() << "count=" << Prefix.size()
                                    << " first=" << unsigned(Prefix.front()));
    const auto Offset = x86PagedCode64UD0OpcodeOffset(Code.data(), Code.size());
    ASSERT_TRUE(Offset);
    EXPECT_EQ(*Offset, Prefix.size());
    DecodedInsn Instruction;
    ASSERT_EQ(
        Decode.decodeOneForLift(Code.data(), Code.size(), 0x1000, Instruction),
        Prefix.size() + X86PagedCode64UD0OpcodeBytes);
    EXPECT_EQ(Instruction.Id, X86_INS_UD0);
    ++Checked;
  };
  for (uint8_t Byte : RepeatedPrefixes)
    for (unsigned PrefixCount = 3;
         PrefixCount <= X86PagedCode64UD0QualifiedPrefixLimit; ++PrefixCount)
      Check(std::vector<uint8_t>(PrefixCount, Byte));
  for (const auto &Pattern : Patterns)
    for (unsigned PrefixCount = 3;
         PrefixCount <= X86PagedCode64UD0QualifiedPrefixLimit; ++PrefixCount)
      Check(
          std::vector<uint8_t>(Pattern.begin(), Pattern.begin() + PrefixCount));
  EXPECT_EQ(Checked, (RepeatedPrefixes.size() + Patterns.size()) *
                         (X86PagedCode64UD0QualifiedPrefixLimit - 2));
  std::vector<uint8_t> Overlong(X86PagedCode64UD0QualifiedPrefixLimit + 1,
                                0x66);
  Overlong.insert(Overlong.end(), {0x0f, 0xff});
  EXPECT_FALSE(x86PagedCode64UD0OpcodeOffset(Overlong.data(), Overlong.size()));
  for (unsigned LockPosition : {0u, 6u, 12u}) {
    std::vector<uint8_t> Locked(X86PagedCode64UD0QualifiedPrefixLimit, 0x66);
    Locked[LockPosition] = 0xf0;
    Locked.insert(Locked.end(), {0x0f, 0xff});
    EXPECT_FALSE(x86PagedCode64UD0OpcodeOffset(Locked.data(), Locked.size()));
  }
}

TEST_F(X64PagedExecutionTest, UD0CrossPageFetchUsesTheBoundProcessor) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.push_back(NeverC);
  const std::vector<std::vector<uint8_t>> EncodedAddresses{
      {0x00}, {0x04, 0x20}, {0x84, 0x20, 0x00, 0x50, 0x34, 0x12}};
  for (unsigned PrefixCount : {0u, 11u, 12u, 13u})
    for (unsigned Form = 0; Form != EncodedAddresses.size(); ++Form) {
      const uint64_t PC = 0x1ffe - Form - PrefixCount;
      const auto &Address = EncodedAddresses[Form];
      const size_t Offset = PC - 0x1000;
      std::vector<uint8_t> Code(
          Offset + PrefixCount + X86PagedCode64UD0OpcodeBytes + Address.size(),
          0x90);
      std::fill_n(Code.begin() + Offset, PrefixCount, 0x66);
      Code[Offset + PrefixCount] = 0x0f;
      Code[Offset + PrefixCount + 1] = 0xff;
      std::copy(Address.begin(), Address.end(),
                Code.begin() + Offset + PrefixCount +
                    X86PagedCode64UD0OpcodeBytes);
      for (const auto &Compiler : Compilers)
        for (bool LLVM : {false, true})
          for (const std::string Optimization : {"-O0", "-O2"}) {
            Run Driver = nullptr;
            ASSERT_NO_FATAL_FAILURE(
                compile(Code, LLVM, Optimization, Driver, {PC}, Compiler));
            for (auto Profile : {X86UD0FetchProfile::OpcodeOnly,
                                 X86UD0FetchProfile::CompleteModRM}) {
              SCOPED_TRACE(testing::Message()
                           << Compiler << ' ' << LLVM << ' ' << Optimization
                           << " address_form=" << Form << " prefixes="
                           << PrefixCount << " profile=" << unsigned(Profile));
              reset(Code);
              State.Core.Core.Core.Core.PC = PC;
              put(0x13010, 0); // ModR/M page is absent.
              Memory = llvm::cantFail(
                  X86PagedMemory::create(RAM, Features, Controls));
              X64PagedExecutionOptions Options;
              Options.UD0Fetch = Profile;
              X64PagedExecution Context(*Memory, Options);
              uint64_t Budget = 1;
              const auto Exit = Driver(
                  &State, reinterpret_cast<uint8_t *>(&Context), &Budget);
              ASSERT_TRUE(Context.lastError().empty()) << Context.lastError();
              EXPECT_EQ(Exit, 7u);
              EXPECT_EQ(Budget, 0u);
              const unsigned Vector =
                  Profile == X86UD0FetchProfile::OpcodeOnly ? 6
                  : x86UD0FetchExceedsInstructionLength(
                        PrefixCount + X86PagedCode64UD0OpcodeBytes + Form)
                      ? 13
                      : 14;
              ASSERT_TRUE(Context.lastResult().Exception);
              EXPECT_EQ(Context.lastResult().Exception->Vector, Vector);
              EXPECT_EQ(Context.lastResult().Exception->ErrorCode,
                        Vector == 14 ? 0x10u : 0u);
              EXPECT_FALSE(Context.lastResult().InstructionCompleted);
              EXPECT_EQ(State.Core.Core.CR2, Vector == 14 ? 0x2000u : 0u);
              EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x4000u + Vector * 16u);
            }
          }
    }
}

TEST_F(X64PagedExecutionTest, UD0AddressExtentUsesLiveBytesAfterTheOpcode) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  for (bool Addr32 : {false, true}) {
    const uint64_t PC = 0x1ffc - Addr32;
    std::vector<uint8_t> Code(PC - 0x1000 + 2 + Addr32, 0x90);
    Code.back() = 0xff;
    Code[Code.size() - 2] = 0x0f;
    if (Addr32)
      Code[Code.size() - 3] = 0x67;
    for (bool LLVM : {false, true}) {
      Run Driver = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, {PC}));
      for (uint8_t ModRM : {uint8_t{0xc0}, uint8_t{0x84}, uint8_t{0x00}})
        for (auto Profile : {X86UD0FetchProfile::OpcodeOnly,
                             X86UD0FetchProfile::CompleteModRM}) {
          SCOPED_TRACE(testing::Message()
                       << "route=" << LLVM << " addr32=" << Addr32 << " modrm="
                       << unsigned(ModRM) << " profile=" << unsigned(Profile));
          reset(Code);
          State.Core.Core.Core.Core.PC = PC;
          // A register-indirect UD0 never resolves or reads its data operand.
          State.Core.Core.Core.Core.GPR[0] = 0x0000800000000000ULL;
          RAM.Memory.front().Bytes[0x1ffe] = ModRM;
          RAM.Memory.front().Bytes[0x1fff] =
              0x20; // SIB before the absent page.
          put(0x13010, 0);
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecutionOptions Options;
          Options.UD0Fetch = Profile;
          X64PagedExecution Context(*Memory, Options);
          uint64_t Budget = 1;
          const auto Exit =
              Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
          ASSERT_TRUE(Context.lastError().empty()) << Context.lastError();
          EXPECT_EQ(Exit, 7u);
          EXPECT_EQ(Budget, 0u);
          ASSERT_TRUE(Context.lastResult().Exception);
          const bool PageFault =
              ModRM == 0x84 && Profile == X86UD0FetchProfile::CompleteModRM;
          EXPECT_EQ(Context.lastResult().Exception->Vector, PageFault ? 14 : 6);
          EXPECT_EQ(Context.lastResult().Exception->ErrorCode,
                    PageFault ? 0x10u : 0u);
          EXPECT_EQ(State.Core.Core.CR2, PageFault ? 0x2000u : 0u);
          EXPECT_EQ(State.Core.Core.Core.Core.PC,
                    0x4000u + (PageFault ? 14u : 6u) * 16u);
        }
    }
  }
}

TEST_F(X64PagedExecutionTest, UD0RefusesMissingOrInvalidFetchProfile) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  std::vector<uint8_t> Long(X86PagedCode64UD0QualifiedPrefixLimit, 0x66);
  Long.insert(Long.end(), {0x0f, 0xff, 0xc0});
  for (const auto &Code :
       {std::vector<uint8_t>{0x0f, 0xff, 0xc0},
        std::vector<uint8_t>{0x67, 0x0f, 0xff, 0xc0},
        std::vector<uint8_t>{0x66, 0x0f, 0xff, 0xc0},
        std::vector<uint8_t>{0x66, 0x67, 0x0f, 0xff, 0xc0}, Long}) {
    for (bool LLVM : {false, true}) {
      Run Driver = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver));
      for (auto Profile : {std::optional<X86UD0FetchProfile>{},
                           std::optional{X86UD0FetchProfile(0xff)}}) {
        reset(Code);
        const auto Before = State;
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        X64PagedExecutionOptions Options;
        Options.UD0Fetch = Profile;
        X64PagedExecution Context(*Memory, Options);
        uint64_t Budget = 1;
        EXPECT_EQ(
            Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget), 5u);
        EXPECT_EQ(Budget, 1u);
        EXPECT_NE(Context.lastError().find("bound instruction-fetch profile"),
                  std::string::npos);
        EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
        EXPECT_EQ(
            Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            RAM.Memory.front().Bytes);
      }
    }
  }
}

TEST_F(X64PagedExecutionTest, AlignmentPriorityRequiresAnExplicitCPUContract) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  const std::vector<uint8_t> Code{0x48, 0x8b, 0x07};
  for (bool LLVM : {false, true}) {
    Run Driver = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver));
    for (auto Priority :
         {std::optional<X64PagedAlignmentPriority>{},
          std::optional{X64PagedAlignmentPriority::BeforePaging},
          std::optional{X64PagedAlignmentPriority::AfterPaging}}) {
      reset(Code);
      auto &Core = State.Core.Core.Core.Core;
      Core.GPR[7] = 0x20001;
      Core.RFlags |= 0x40000;
      Controls.CR0 |= 0x40000;
      State.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
      State.Core.SS = {0, UINT32_MAX, 35, 0xc0f3};
      State.Core.Core.CR2 = 0x1234;
      put(0x13100, 0);
      const auto Before = State;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Context(*Memory, {Priority});
      uint64_t Budget = 1;
      const auto Exit =
          Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
      if (!Priority) {
        EXPECT_EQ(Exit, 5u);
        EXPECT_EQ(Budget, 1u);
        EXPECT_NE(Context.lastError().find("bound fault priority"),
                  std::string::npos);
        EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
        EXPECT_EQ(
            Memory->physicalMemory().snapshotMemoryRegions().front().Bytes,
            RAM.Memory.front().Bytes);
      } else {
        EXPECT_TRUE(Context.lastError().empty());
        EXPECT_EQ(Exit, 7u);
        EXPECT_EQ(Budget, 0u);
        const bool First = *Priority == X64PagedAlignmentPriority::BeforePaging;
        EXPECT_EQ(Core.PC, First ? 0x4110u : 0x40e0u);
        EXPECT_EQ(State.Core.Core.CR2, First ? 0x1234u : 0x20001u);
        EXPECT_EQ(Core.GPR[0], Before.Core.Core.Core.Core.GPR[0]);
      }
    }
  }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest, UD0FetchPriorityMatchesOriginalHybridCPU) {
  if (!neverd::test::hostCpuIsIntel() || !hasCrossTargetClang())
    GTEST_SKIP() << "Intel KVM and test Clang are required";
  X64PagedNativeCPU CPU;
  CPU.pinForReturnComparison();
  if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
    GTEST_SKIP() << "UD0 fetch policy is unqualified for this core type";
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  const auto Profile = CPU.ReturnCoreType == 0x20
                           ? X86UD0FetchProfile::OpcodeOnly
                           : X86UD0FetchProfile::CompleteModRM;
  RecordProperty("core_type", CPU.ReturnCoreType);
  const std::vector<std::vector<uint8_t>> EncodedAddresses{
      {0x00}, {0x04, 0x20}, {0x84, 0x20, 0x00, 0x50, 0x34, 0x12}};
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.push_back(NeverC);
  std::vector<std::vector<uint8_t>> Prefixes{
      {},           {0x66},       {0x67},      {0xf2}, {0xf3},
      {0x2e},       {0x65},       {0x40},      {0x4f}, {0x66, 0x66},
      {0x66, 0x67}, {0x67, 0x66}, {0x67, 0x67}};
  for (unsigned Count : {3u, 11u, 12u, 13u})
    Prefixes.emplace_back(Count, 0x66);
  Prefixes.push_back({0x66, 0x67, 0xf2, 0x64, 0x4f, 0x2e, 0xf3});
  Prefixes.push_back(
      {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49});
  Prefixes.push_back({0x66, 0x67, 0xf2, 0x64, 0x4f, 0x2e, 0xf3, 0x40, 0x65,
                      0x3e, 0x48, 0x36, 0x26});
  Prefixes.push_back({0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
                      0x49, 0x4a, 0x4b, 0x4c});
  unsigned Comparisons = 0;
  for (const auto &Prefix : Prefixes) {
    for (unsigned Form = 0; Form != EncodedAddresses.size(); ++Form) {
      const unsigned MissingByte =
          Prefix.size() + X86PagedCode64UD0OpcodeBytes + Form;
      const unsigned Vector = Profile == X86UD0FetchProfile::OpcodeOnly ? 6
                              : x86UD0FetchExceedsInstructionLength(MissingByte)
                                  ? 13
                                  : 14;
      const uint64_t PC = 0x1ffe - Form - Prefix.size();
      const auto &Address = EncodedAddresses[Form];
      const size_t Offset = PC - 0x1000;
      std::vector<uint8_t> Code(Offset + Prefix.size() + 2 + Address.size(),
                                0x90);
      std::copy(Prefix.begin(), Prefix.end(), Code.begin() + Offset);
      Code[Offset + Prefix.size()] = 0x0f;
      Code[Offset + Prefix.size() + 1] = 0xff;
      std::copy(Address.begin(), Address.end(),
                Code.begin() + Offset + Prefix.size() + 2);
      reset(Code);
      State.Core.Core.Core.Core.PC = PC;
      State.Core.Core.Core.Core.GPR[0] = 0x0000800000000000ULL;
      put(0x13010, 0);
      X64ContinuationStateV6 Native;
      bool Handler = false;
      ASSERT_NO_FATAL_FAILURE(CPU.execute(
          RAM.Memory.front().Bytes, State, Controls.CR0,
          Prefix.size() + 2 + Address.size(), Native, Handler, true));
      ASSERT_TRUE(Handler);
      EXPECT_EQ(Native.Core.Core.Core.Core.PC, 0x4000u + Vector * 16u);
      EXPECT_EQ(Native.Core.Core.CR2, Vector == 14 ? 0x2000u : 0u);
      const uint64_t NativeSP = Native.Core.Core.Core.Core.GPR[4];
      const bool HasErrorCode = Vector != 6;
      ASSERT_LE(NativeSP + (HasErrorCode ? 16u : 8u),
                uint64_t(X64PagedNativeCPU::Size));
      const auto FrameWord = [&](unsigned Offset) {
        uint64_t Value;
        std::memcpy(&Value, CPU.RAM + NativeSP + Offset, sizeof(Value));
        return Value;
      };
      EXPECT_EQ(FrameWord(HasErrorCode ? 8 : 0), PC);
      if (HasErrorCode)
        EXPECT_EQ(FrameWord(0), Vector == 14 ? 0x10u : 0u);
      for (const auto &Compiler : Compilers)
        for (bool LLVM : {false, true})
          for (const std::string Optimization : {"-O0", "-O2"}) {
            SCOPED_TRACE(testing::Message()
                         << "core=" << CPU.ReturnCoreType << " form=" << Form
                         << " prefix=" << Prefix.size() << ' ' << Compiler
                         << ' ' << LLVM << ' ' << Optimization);
            Run Driver = nullptr;
            ASSERT_NO_FATAL_FAILURE(
                compile(Code, LLVM, Optimization, Driver, {PC}, Compiler));
            reset(Code);
            State.Core.Core.Core.Core.PC = PC;
            State.Core.Core.Core.Core.GPR[0] = 0x0000800000000000ULL;
            put(0x13010, 0);
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecutionOptions Options;
            Options.UD0Fetch = Profile;
            X64PagedExecution Context(*Memory, Options);
            uint64_t Budget = 1;
            const auto Exit =
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
            ASSERT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Exit, 7u);
            EXPECT_EQ(Budget, 0u);
            ASSERT_TRUE(Context.lastResult().Exception);
            EXPECT_EQ(Context.lastResult().Exception->Vector, Vector);
            EXPECT_EQ(Context.lastResult().Exception->ErrorCode,
                      Vector == 14 ? 0x10u : 0u);
            EXPECT_FALSE(Context.lastResult().InstructionCompleted);
            const auto &Core = State.Core.Core.Core.Core;
            const auto &Original = Native.Core.Core.Core.Core;
            EXPECT_EQ(Core.PC, Original.PC);
            EXPECT_EQ(Core.RFlags, Original.RFlags);
            for (unsigned I = 0; I != 16; ++I)
              EXPECT_EQ(Core.GPR[I], Original.GPR[I]) << I;
            EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
            EXPECT_EQ(State.Core.CS.Selector, Native.Core.CS.Selector);
            EXPECT_EQ(State.Core.SS.Selector, Native.Core.SS.Selector);
            const auto Actual =
                Memory->physicalMemory().snapshotMemoryRegions();
            EXPECT_EQ(
                std::memcmp(Actual.front().Bytes.data(), CPU.RAM, 0x10000), 0);
            EXPECT_EQ(std::memcmp(Actual.front().Bytes.data() + 0x14000,
                                  CPU.RAM + 0x14000,
                                  X64PagedNativeCPU::Size - 0x14000),
                      0);
            ++Comparisons;
          }
    }
  }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_F(X64PagedExecutionTest, SoftwareEntryAtTheLastCanonicalInstructionByte) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (IsSkipped())
    return;
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  constexpr uint64_t Base = 0x7ffffffff000;
  unsigned Comparisons = 0;
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *NeverC = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.push_back(NeverC);
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0xcc}, {0xcd, 0x80}})
    for (const auto &Compiler : Compilers)
      for (bool LLVM : {false, true})
        for (const std::string O : {"-O0", "-O2"}) {
          SCOPED_TRACE(testing::Message() << Bytes.size() << ' ' << Compiler
                                          << ' ' << LLVM << ' ' << O);
          std::vector<uint8_t> Code(4096, 0x90);
          std::copy(Bytes.begin(), Bytes.end(), Code.end() - Bytes.size());
          const uint64_t PC = Base + Code.size() - Bytes.size();
          Run Driver = nullptr;
          ASSERT_NO_FATAL_FAILURE(
              compile(Code, LLVM, O, Driver, {PC}, Compiler, Base));
          reset(Code);
          State.Core.Core.Core.Core.PC = PC;
          put(0x107f8, 0x14007);
          put(0x14ff8, 0x15007);
          put(0x15ff8, 0x16007);
          put(0x16ff8, 0x1007);
          X64ContinuationStateV6 Native;
          bool Handler = false;
          ASSERT_NO_FATAL_FAILURE(CPU.execute(RAM.Memory.front().Bytes, State,
                                              Controls.CR0, Bytes.size(),
                                              Native, Handler, true));
          ASSERT_TRUE(Handler);
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Context(*Memory);
          uint64_t Budget = 1;
          const auto Exit =
              Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
          EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
          EXPECT_EQ(Exit, 7u);
          EXPECT_EQ(Budget, 0u);
          EXPECT_TRUE(Context.lastResult().InstructionCompleted);
          const auto &Core = State.Core.Core.Core.Core;
          const auto &Expected = Native.Core.Core.Core.Core;
          EXPECT_EQ(Core.PC, Expected.PC);
          EXPECT_EQ(Core.RFlags, Expected.RFlags);
          for (unsigned I = 0; I != 16; ++I)
            EXPECT_EQ(Core.GPR[I], Expected.GPR[I]);
          EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
          const auto Actual = Memory->physicalMemory().snapshotMemoryRegions();
          EXPECT_EQ(std::memcmp(Actual.front().Bytes.data(), CPU.RAM, 0x10000),
                    0);
          EXPECT_EQ(std::memcmp(Actual.front().Bytes.data() + 0x17000,
                                CPU.RAM + 0x17000,
                                X64PagedNativeCPU::Size - 0x17000),
                    0);
          ++Comparisons;
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

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
    bool Software = false;
  };
  std::vector<Case> Cases{{"load64", {0x48, 0x8b, 0x07}},
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
                          {"int3", {0xcc}, false, true},
                          {"int0", {0xcd, 0x00}, false, true},
                          {"int8", {0xcd, 0x08}, false, true},
                          {"int14", {0xcd, 0x0e}, false, true},
                          {"int128", {0xcd, 0x80}, false, true},
                          {"split-fetch", {0x48, 0x8b, 0x07}}};
  // Exercise every original software vector, including service-shaped LowIR,
  // without interpreting its number as an architectural fault class.
  for (unsigned V = 0; V != 256; ++V)
    if (V != 0 && V != 8 && V != 14 && V != 128)
      Cases.insert(Cases.end() - 1, {"int-n", {0xcd, uint8_t(V)}, false, true});
  std::vector<uint8_t> Code(0x1001, 0x90);
  std::vector<uint64_t> PCs;
  for (unsigned I = 0; I != Cases.size(); ++I) {
    const uint64_t PC = I + 1 == Cases.size() ? 0x1ffe : 0x1000 + I * 8;
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
  unsigned Comparisons = 0, FaultComparisons = 0, HandlerComparisons = 0;
  unsigned SoftwareComparisons = 0, SoftwareCompletions = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, O, Driver, PCs, Compiler));
        for (unsigned I = 0; I != Cases.size(); ++I)
          for (unsigned Variant = 0; Variant != 15; ++Variant) {
            SCOPED_TRACE(testing::Message()
                         << Compiler << ' ' << LLVM << ' ' << O << ' '
                         << Cases[I].Name << " case=" << I
                         << " variant=" << Variant);
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
            if (Variant == 4 || Variant >= 12 ||
                (Variant == 10 && Cases[I].Software)) {
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
              for (unsigned V = 0; V != 256; ++V)
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
            if (Variant == 10)
              Core.RFlags |= 0x10000;
            if (Variant == 11 && !Cases[I].Software)
              put(0x13000 + (PCs[I] / 4096) * 8,
                  (PCs[I] & ~uint64_t(4095)) | 7 | (uint64_t(1) << 63));
            if (Variant == 12)
              put(0x13100, 0); // Competing #PF and scalar alignment check.
            if (Variant == 13)
              Core.GPR[7] = Core.GPR[5] = 0x800000000001;
            if (Variant == 14) {
              Core.GPR[7] = Core.GPR[5] = 0x20fff;
              put(0x13108, 0);
            }
            if (Cases[I].Software) {
              const unsigned V =
                  Cases[I].Bytes.size() == 1 ? 3 : Cases[I].Bytes.back();
              if (Variant == 10 || Variant == 11)
                put(0x6000 + V * 16,
                    (0x4000 + V * 16) |
                        (uint64_t(Variant == 11 && V == 11 ? 0 : 8) << 16) |
                        (uint64_t(Variant == 10 || V == 11 ? 0xee : 0x0e)
                         << 40));
              if (Variant == 12) {
                if (V == 128) {
                  // Only this high software vector's gate crosses into the
                  // absent page. #PF's gate and handler stack remain present.
                  std::memmove(RAM.Memory.front().Bytes.data() + 0x6800,
                               RAM.Memory.front().Bytes.data() + 0x6000, 4096);
                  State.IDTR.Base = 0x6800;
                  put(0x13038, 0);
                } else if (V == 13)
                  put(0x6000 + V * 16, (0x4000 + V * 16) | (uint64_t(8) << 16) |
                                           (uint64_t(0x6e) << 40));
                else
                  put(0x6000 + V * 16,
                      (0x4000 + V * 16) | (uint64_t(0xee) << 40));
              }
            }
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
            ASSERT_NO_FATAL_FAILURE(CPU.execute(
                RAM.Memory.front().Bytes, State, Controls.CR0,
                Cases[I].Bytes.size(), Native, Fault, Cases[I].Software));
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
            X64PagedExecution Context(
                *Memory, {X64PagedAlignmentPriority::BeforePaging});
            uint64_t Budget = 1;
            const auto Exit =
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget);
            ASSERT_TRUE(Context.lastError().empty()) << Context.lastError();
            ASSERT_NE(Exit, 5u);
            EXPECT_EQ(Budget, 0u);
            EXPECT_EQ(Exit == 7, Fault);
            const bool SoftwareCompleted = Cases[I].Software && Variant != 4 &&
                                           Variant != 8 && Variant != 11 &&
                                           Variant < 12;
            EXPECT_EQ(Context.lastResult().InstructionCompleted,
                      !Fault || SoftwareCompleted);
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
            HandlerComparisons += Fault;
            FaultComparisons += Fault && !SoftwareCompleted;
            SoftwareComparisons += Cases[I].Software;
            SoftwareCompletions += SoftwareCompleted;
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("fault_cpu_comparisons", FaultComparisons);
  RecordProperty("handler_cpu_comparisons", HandlerComparisons);
  RecordProperty("software_interrupt_cpu_comparisons", SoftwareComparisons);
  RecordProperty("software_completion_cpu_comparisons", SoftwareCompletions);
}
#endif
#include "X64PagedDebugTests.inc"
#include "X64PagedDescriptorBoundaryFixture.inc"
#include "X64PagedDescriptorCanonicalityTests.inc"
#include "X64PagedEventDescriptorTests.inc"
#include "X64PagedExecutionBreakpointTests.inc"
#include "X64PagedFarPointerTests.inc"
#include "X64PagedFarReturnTests.inc"
#include "X64PagedPortIOBoundaryTests.inc"
#include "X64PagedPortIOTests.inc"
#include "X64PagedRegisterDebugTests.inc"
#include "X64PagedRepeatedStringBoundaryTests.inc"
#include "X64PagedRepeatedStringTests.inc"
#include "X64PagedReturnContinuationTests.inc"
#include "X64PagedReturnDescriptorTests.inc"
#include "X64PagedReturnTests.inc"
#include "X64PagedScheduleTests.inc"
#include "X64PagedSegmentContinuationTests.inc"
#include "X64PagedSegmentMoveBoundaryTests.inc"
#include "X64PagedSegmentMoveTests.inc"
#include "X64PagedSegmentPushTests.inc"
#include "X64PagedWatchpointTests.inc"

// Accessed-update cases extend the event descriptor fixture above.
#include "X64PagedEventAccessedTests.inc"

// These cases reuse the debug/watchpoint fixtures above.
#include "X64PagedBranchDebugTests.inc"
#include "X64PagedDebugFunctionTests.inc"
#include "X86ModeExecutionTests.inc"
#include "X86ModeInterruptTests.inc"
#include "X86ModeMachineCheckTests.inc"
#include "X86ModeSegmentTests.inc"
#include "X86ModeSgxNotificationTests.inc"
#include "X86ModeSgxTests.inc"
#include "X86ModeSmmTests.inc"
#include "X86ModeStartupTests.inc"
#include "X86ModeSysenterTests.inc"
#include "X86ModeTransitionTests.inc"

// Native comparisons extend the selector fixture above.
#include "X86ModeSegmentNativeTests.inc"

// These cases reuse the mixed-mode native fixture above.
#include "X86ModeDebugRegisterTests.inc"
#include "X86ModeMachineStatusTests.inc"

} // namespace
