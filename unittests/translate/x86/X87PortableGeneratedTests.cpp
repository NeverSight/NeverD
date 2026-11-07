//===- X87PortableGeneratedTests.cpp - Execute generated portable leaves --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compile owned input bytes through all three routes and execute their exact
/// alternate C interface against explicit state and checked guest memory.
//===----------------------------------------------------------------------===//

#include "X87PortableOriginal.h"
#include "X87PortableTestImage.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/med/X86/PortableX87Function.h"
#include "neverd/translate/X86/X87PortableExecution.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::test;
using namespace neverd::translate;

namespace {

std::array<uint8_t, 10> binary80(uint64_t Significand, uint16_t Exponent) {
  std::array<uint8_t, 10> Bytes{};
  for (unsigned I = 0; I < 8; ++I)
    Bytes[I] = Significand >> (I * 8);
  Bytes[8] = Exponent;
  Bytes[9] = Exponent >> 8;
  return Bytes;
}

PortableX87StateV1 environment(uint16_t Control = 0x037f, unsigned Top = 5) {
  PortableX87StateV1 State;
  State.EnvironmentKnown = 1;
  State.Image[0] = Control;
  State.Image[1] = Control >> 8;
  State.Image[3] = Top << 3;
  for (unsigned I = 32; I < 160; ++I)
    State.Image[I] = uint8_t(I ^ 0xa5);
  return State;
}

uint16_t word(const PortableX87StateV1 &State, unsigned Offset) {
  return State.Image[Offset] | uint16_t(State.Image[Offset + 1]) << 8;
}

uint64_t address(const PortableX87StateV1 &State, unsigned Offset) {
  uint64_t Value = 0;
  for (unsigned I = 0; I < 8; ++I)
    Value |= uint64_t(State.Image[Offset + I]) << (I * 8);
  return Value;
}

std::unique_ptr<GuestMemoryRuntime>
memory(std::vector<GuestMemoryRegion> Regions) {
  auto Guest =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  Guest.Memory = std::move(Regions);
  return llvm::cantFail(GuestMemoryRuntime::create(Guest));
}

std::unique_ptr<GuestMemoryRuntime> inputs(bool Nan = false, bool Fault = false,
                                           bool Executable = false) {
  const auto A = binary80(Nan ? 0xc000000000000123 : 0x8000000000000001,
                          Nan ? 0x7fff : 0x3fff);
  const auto B = binary80(0x8000000000000000, 0x3ffe);
  std::vector<GuestMemoryRegion> Regions{
      // These numeric guest addresses deliberately collide with the source
      // entry and the prepared private control label. Neither is a host
      // pointer.
      {1, MemoryPermission::Read, 0, {B.begin(), B.end()}},
      {0x1000, MemoryPermission::Read, 0, {A.begin(), A.end()}},
      {0x3000,
       Executable ? MemoryPermission::Write | MemoryPermission::Execute
                  : MemoryPermission::Write,
       Executable ? 7u : 0u, std::vector<uint8_t>(Fault ? 10 : 12, 0xa5)}};
  if (Fault)
    Regions.push_back({0x300a, MemoryPermission::Read, 0, {0x5a, 0xa5}});
  return memory(std::move(Regions));
}

std::string rawState(const PortableX87StateV1 &State) {
  return llvm::toHex(
      llvm::ArrayRef(reinterpret_cast<const uint8_t *>(&State), sizeof(State)));
}

void record(llvm::raw_ostream &Out, llvm::StringRef Artifact,
            llvm::StringRef Case, const PortableX87StateV1 &Before,
            const PortableX87StateV1 &After, uint32_t Status, uint64_t Result,
            const GuestMemoryRuntime &Memory,
            const PortableX87ExecutionV1 &Execution,
            const X87OriginalObservation *Original = nullptr,
            uint16_t NativeStatusMask = 0xbaff) {
  llvm::json::Array Regions;
  for (const auto &Region : Memory.snapshotMemoryRegions())
    Regions.push_back(llvm::json::Object{
        {"address", llvm::utohexstr(Region.Address)},
        {"generation", Region.Generation},
        {"permissions", static_cast<unsigned>(Region.Permissions)},
        {"bytes", llvm::toHex(Region.Bytes)}});
  llvm::json::Value Reference = nullptr;
  if (Original) {
    const auto &Image = Original->Image;
    // Defined status bits depend on the instruction. Relocated host pointers
    // are not compared here.
    Reference = llvm::json::Object{
        {"return_defined",
         llvm::utohexstr(Original->Return & ~UINT64_C(0x4500))},
        {"control", unsigned(Image[0]) | unsigned(Image[1]) << 8},
        {"status_defined",
         (unsigned(Image[2]) | unsigned(Image[3]) << 8) & NativeStatusMask},
        {"tags", unsigned(Image[4])},
        {"retained_result",
         llvm::toHex(llvm::ArrayRef(Image).slice(32 + 7 * 16, 10))},
        {"retained_second",
         llvm::toHex(llvm::ArrayRef(Image).slice(32 + 6 * 16, 10))},
        {"memory", llvm::toHex(Original->Memory)}};
  }
  Out << llvm::json::Value(llvm::json::Object{
             {"artifact", Artifact},
             {"case", Case},
             {"before", rawState(Before)},
             {"after", rawState(After)},
             {"status", Status},
             {"instruction_address",
              llvm::utohexstr(Execution.lastInstructionAddress())},
             {"data_address", llvm::utohexstr(Execution.lastDataAddress())},
             {"instruction_completed",
              Execution.lastStep().InstructionCompleted},
             {"has_scalar_result", Execution.lastStep().HasScalarResult},
             {"memory_access", Execution.lastStep().MemoryAccess.has_value()},
             {"return", llvm::utohexstr(Result)},
             {"memory", std::move(Regions)},
             {"native_reference", std::move(Reference)}})
      << '\n';
}

struct Loaded {
  llvm::sys::DynamicLibrary Library;
  ~Loaded() {
    if (Library.isValid())
      llvm::sys::DynamicLibrary::closeLibrary(Library);
  }
};

using Leaf = uint32_t (*)(uint64_t, uint64_t, uint64_t, void *, uint64_t *);
using Control = uint32_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, void *,
                             uint64_t *);

std::vector<uint8_t> resetBytes(unsigned Kind) {
  // lea rax,[rdi+rsi]; add rax,rdx; FNOP/EMMS/FEMMS; ret.
  // Keep an ordinary three-integer ABI independent of the x87 bank.
  return {0x48,
          0x8d,
          0x04,
          0x37,
          0x48,
          0x01,
          0xd0,
          uint8_t(Kind == 0 ? 0xd9 : 0x0f),
          uint8_t(Kind == 0   ? 0xd0
                  : Kind == 1 ? 0x77
                              : 0x0e),
          0xc3};
}

void executeReset(void *Symbol, unsigned Kind, llvm::ArrayRef<uint8_t> Bytes,
                  llvm::StringRef Artifact, llvm::raw_ostream &Records) {
  auto Original = X87PortableOriginal::create(Bytes);
  ASSERT_TRUE(static_cast<bool>(Original))
      << llvm::toString(Original.takeError());
  auto Memory = memory({});
  const auto Invoke = [&](PortableX87ExecutionV1 *Execution, uint64_t *Result) {
    return reinterpret_cast<Leaf>(Symbol)(0x1000, 1, 0x3001, Execution, Result);
  };
  unsigned Observations = 0, NativeObservations = 0;
  for (unsigned Top = 0; Top != 8; ++Top)
    for (uint8_t Tags : {0, 0x55, 0xff})
      for (uint8_t Known : {0, 0x96, 0xff}) {
        auto State = environment(0x37f, Top);
        State.Image[2] = 0x25;
        State.Image[3] |= 0x47;
        State.Image[4] = Tags;
        State.KnownPayload = Known;
        State.Image[6] = 0x71;
        State.Image[7] = 5;
        State.Image[8] = 0x45;
        State.Image[16] = 0x67;
        const auto Before = State;
        auto Expected = Before;
        if (Kind == 0) {
          for (unsigned I = 0; I != 8; ++I)
            Expected.Image[8 + I] = (kPortableX87FixtureEntry + 7) >> (8 * I);
        } else {
          Expected.Image[3] = 0x47;
          Expected.Image[4] = 0;
          for (unsigned Physical = 0; Physical != 8; ++Physical)
            std::copy_n(Before.Image.begin() + 32 +
                            ((Physical + 8 - Top) & 7) * 16,
                        16, Expected.Image.begin() + 32 + Physical * 16);
        }
        if (Kind == 2)
          Expected.KnownPayload = 0;
        PortableX87ExecutionV1 Execution(State, *Memory);
        uint64_t Result = UINT64_MAX;
        const auto Status = Invoke(&Execution, &Result);
        ASSERT_EQ(Status, NVD_X87_STATUS_COMPLETED);
        EXPECT_EQ(Result, 0x4002U);
        EXPECT_EQ(std::memcmp(&State, &Expected, sizeof(State)), 0);
        EXPECT_TRUE(Execution.lastStep().InstructionCompleted);
        EXPECT_FALSE(Execution.lastStep().MemoryAccess);
        EXPECT_FALSE(Execution.lastStep().HasScalarResult);
        std::optional<X87OriginalObservation> Native;
        // FEMMS is absent on this Intel-compatible native reference profile.
        // Its undefined payloads are checked through provenance, never by
        // comparing an arbitrary hardware-selected value.
        if (*Original && Kind != 2 && Known == 0xff) {
          Native = (*Original)->run(Before, 0, 0, {}, {});
          const unsigned StatusMask = Kind == 0 ? 0xb8ff : 0xffff;
          const auto &Image = Native->Image;
          EXPECT_EQ(unsigned(Image[0]) | unsigned(Image[1]) << 8,
                    word(State, 0));
          EXPECT_EQ((unsigned(Image[2]) | unsigned(Image[3]) << 8) & StatusMask,
                    word(State, 2) & StatusMask);
          EXPECT_EQ(Image[4], State.Image[4]);
          for (unsigned I = 0; I != 8; ++I)
            EXPECT_TRUE(std::equal(Image.begin() + 32 + I * 16,
                                   Image.begin() + 42 + I * 16,
                                   State.Image.begin() + 32 + I * 16));
          ++NativeObservations;
        }
        const auto Case = "reset-" + std::to_string(Top) + "-" +
                          std::to_string(Tags) + "-" + std::to_string(Known);
        record(Records, Artifact, Case, Before, State, Status, Result, *Memory,
               Execution, Native ? &*Native : nullptr,
               Kind == 0 ? 0xb8ff : 0xffff);
        ++Observations;
      }
  for (unsigned Refusal = 0; Refusal != 3; ++Refusal) {
    auto State = environment();
    State.KnownPayload = State.Image[4] = 0xff;
    if (Refusal == 0)
      State.EnvironmentKnown = 0;
    else if (Refusal == 1) {
      State.Image[0] &= ~1U;
      State.Image[2] = 0x81;
    } else {
      State.Image[1] = 1; // Reserved precision control.
    }
    const auto Before = State;
    PortableX87ExecutionV1 Execution(State, *Memory);
    uint64_t Result = UINT64_MAX;
    const auto Status = Invoke(&Execution, &Result);
    const unsigned Expected[] = {NVD_X87_STATUS_UNKNOWN_ENVIRONMENT,
                                 NVD_X87_STATUS_PENDING_EXCEPTION,
                                 NVD_X87_STATUS_INVALID_CONTROL};
    EXPECT_EQ(Status, Expected[Refusal]);
    EXPECT_EQ(Result, UINT64_MAX);
    EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
    EXPECT_FALSE(Execution.lastStep().InstructionCompleted);
    EXPECT_FALSE(Execution.lastStep().MemoryAccess);
    record(Records, Artifact, "reset-refusal-" + std::to_string(Refusal),
           Before, State, Status, Result, *Memory, Execution);
  }
  EXPECT_EQ(Observations, 72U);
  EXPECT_EQ(NativeObservations, *Original && Kind != 2 ? 24U : 0U);
}

void execute(void *Symbol, unsigned Shape, llvm::ArrayRef<uint8_t> Bytes,
             llvm::StringRef Artifact, llvm::raw_ostream &Records) {
  auto Original = X87PortableOriginal::create(Bytes);
  ASSERT_TRUE(static_cast<bool>(Original))
      << llvm::toString(Original.takeError());
  const auto A = binary80(0x8000000000000001, 0x3fff);
  const auto B = binary80(0x8000000000000000, 0x3ffe);
  const auto Invoke = [&](PortableX87ExecutionV1 *Execution, uint64_t *Result,
                          uint64_t Count = 1) {
    return Shape == 0 ? reinterpret_cast<Leaf>(Symbol)(0x1000, 1, 0x3001,
                                                       Execution, Result)
                      : reinterpret_cast<Control>(Symbol)(
                            0x1000, 1, 0x3001, Count, Execution, Result);
  };
  for (unsigned Top = 0; Top < 8; ++Top)
    for (unsigned PC : {0u, 2u, 3u})
      for (unsigned RC = 0; RC < 4; ++RC)
        for (uint64_t Count : {1u, 2u}) {
          if (Shape == 0 && Count == 2)
            continue;
          const auto Case = "exact-" + std::to_string(Top) + "-" +
                            std::to_string(PC) + "-" + std::to_string(RC) +
                            "-" + std::to_string(Count);
          SCOPED_TRACE(Case);
          auto State = environment(0x7f | PC << 8 | RC << 10, Top);
          const auto Before = State;
          auto Memory = inputs();
          PortableX87ExecutionV1 Execution(State, *Memory);
          uint64_t Result = 0xfedcba9876543210;
          const auto Status = Invoke(&Execution, &Result, Count);
          std::optional<X87OriginalObservation> Native;
          if (*Original)
            Native = (*Original)->run(Before, Shape, Count, A, B);
          record(Records, Artifact, Case, Before, State, Status, Result,
                 *Memory, Execution, Native ? &*Native : nullptr);
          ASSERT_EQ(Status, 0u);
          uint64_t Mantissa = 0xc000000000000000;
          if (PC == 3)
            Mantissa |= 1;
          else if (RC == 2)
            Mantissa |= uint64_t{1} << (PC == 2 ? 11 : 40);
          const auto Expected =
              binary80(Mantissa, Shape == 2 && Count % 2 ? 0xbfff : 0x3fff);
          const auto Output = Memory->snapshotMemoryRegions()[2].Bytes;
          EXPECT_EQ(Output.front(), 0xa5);
          EXPECT_EQ(Output.back(), 0xa5);
          if (Native) {
            EXPECT_TRUE(std::equal(Output.begin(), Output.end(),
                                   Native->Memory.begin()));
            EXPECT_EQ(Result & ~UINT64_C(0x4500),
                      Native->Return & ~UINT64_C(0x4500));
            EXPECT_EQ(word(State, 0), unsigned(Native->Image[0]) |
                                          unsigned(Native->Image[1]) << 8);
            EXPECT_EQ(
                word(State, 2) & ~0x4500u,
                (unsigned(Native->Image[2]) | unsigned(Native->Image[3]) << 8) &
                    ~0x4500u);
            EXPECT_EQ(State.Image[4], Native->Image[4]);
            for (unsigned Logical : {6u, 7u})
              EXPECT_TRUE(
                  std::equal(State.Image.begin() + 32 + Logical * 16,
                             State.Image.begin() + 42 + Logical * 16,
                             Native->Image.begin() + 32 + Logical * 16));
          }
          EXPECT_TRUE(
              std::equal(Expected.begin(), Expected.end(), Output.begin() + 1));
          const uint16_t StatusWord = (Top << 11) | (PC != 3 ? 0x20u : 0u);
          EXPECT_EQ(Result, StatusWord + (Shape == 1 ? 1u : 0u));
          EXPECT_EQ(word(State, 0), 0x7f | PC << 8 | RC << 10);
          EXPECT_EQ(word(State, 2), StatusWord);
          EXPECT_EQ(State.Image[4], 0);
          EXPECT_EQ(State.KnownPayload,
                    (1u << ((Top + 7) & 7)) | (1u << ((Top + 6) & 7)));
          EXPECT_EQ(address(State, 8), 0x100du + (Shape == 2 ? 7 : 0));
          EXPECT_EQ(address(State, 16), 0x3001u);
          EXPECT_EQ(Execution.lastInstructionAddress(),
                    0x100fu + (Shape == 2 ? 7 : 0));
          EXPECT_EQ(Execution.lastDataAddress(), 0u);
          EXPECT_TRUE(Execution.lastStep().HasScalarResult);
          EXPECT_FALSE(Execution.lastStep().MemoryAccess);
        }
  if (Shape == 1) {
    auto State = environment();
    auto Memory = inputs();
    PortableX87ExecutionV1 Execution(State, *Memory);
    uint64_t Result = 0xfedcba9876543210;
    const auto Before = State;
    const auto Status = Invoke(&Execution, &Result, 0);
    record(Records, Artifact, "untaken-branch", Before, State, Status, Result,
           *Memory, Execution);
    EXPECT_EQ(Status, 0u);
    EXPECT_EQ(Result, 5u << 11);
  }
  for (unsigned Kind = 0; Kind < 7; ++Kind) {
    SCOPED_TRACE(Kind);
    auto State = environment();
    if (Kind == 0)
      State.EnvironmentKnown = 0;
    if (Kind == 1) {
      State.Image[0] &= ~1u;
      State.Image[2] = 1;
    }
    if (Kind == 2)
      State.Image[1] = 1; // Reserved PC.
    if (Kind == 3) {
      State.Image[4] = 0xff;
      State.KnownPayload = 0xff;
    }
    const auto Before = State;
    auto Memory = inputs(Kind == 4, Kind == 5, Kind == 6);
    const auto MemoryBefore = Memory->snapshotMemoryRegions();
    PortableX87ExecutionV1 Execution(State, *Memory);
    uint64_t Result = 0xfedcba9876543210;
    const auto Status = Invoke(&Execution, &Result);
    const uint32_t Expected[] = {2, 6, 5, 9, 10, 12, 13};
    record(Records, Artifact, "refusal-" + std::to_string(Kind), Before, State,
           Status, Result, *Memory, Execution);
    EXPECT_EQ(Status, Expected[Kind]);
    EXPECT_EQ(Result, 0xfedcba9876543210u);
    EXPECT_FALSE(Execution.lastStep().HasScalarResult);
    EXPECT_EQ(Execution.lastStep().InstructionCompleted, Kind == 6);
    const auto Regions = Memory->snapshotMemoryRegions();
    if (Kind < 4) {
      EXPECT_EQ(rawState(State), rawState(Before));
      EXPECT_EQ(Execution.lastInstructionAddress(), 0x1007u);
      EXPECT_EQ(Execution.lastStep().MemoryAccess.has_value(), Kind == 3);
      if (Kind == 3) {
        ASSERT_TRUE(Execution.lastStep().MemoryAccess);
        EXPECT_EQ(Execution.lastStep().MemoryAccess->Status,
                  GuestMemoryAccessStatus::Completed);
      }
    } else if (Kind == 4) {
      EXPECT_EQ(word(State, 2), 3u << 11);
      EXPECT_EQ(State.Image[4], 0x18u);
      EXPECT_EQ(Execution.lastInstructionAddress(), 0x100bu);
      EXPECT_FALSE(Execution.lastStep().MemoryAccess);
    } else if (Kind == 5) {
      EXPECT_EQ(word(State, 2), 4u << 11);
      EXPECT_EQ(State.Image[4], 0x10u);
      EXPECT_EQ(address(State, 8), 0x100bu + (Shape == 2 ? 2 : 0));
      ASSERT_TRUE(Execution.lastStep().MemoryAccess);
      ASSERT_TRUE(Execution.lastStep().MemoryAccess->Fault);
      EXPECT_EQ(Execution.lastStep().MemoryAccess->Fault->Exit.AccessWidthBits,
                80u);
      EXPECT_EQ(Execution.lastStep().MemoryAccess->Fault->Exit.Address,
                0x3001u);
    } else {
      EXPECT_EQ(word(State, 2), 5u << 11);
      EXPECT_EQ(State.Image[4], 0u);
      EXPECT_EQ(Regions[2].Generation, 8u);
      EXPECT_EQ(address(State, 8), 0x100du + (Shape == 2 ? 7 : 0));
    }
    if (Kind != 6)
      for (size_t I = 0; I != Regions.size(); ++I)
        EXPECT_EQ(Regions[I].Bytes, MemoryBefore[I].Bytes);
  }
  auto State = environment();
  const auto Before = State;
  auto Memory = inputs();
  PortableX87ExecutionV1 Execution(State, *Memory);
  uint64_t Result = 0xfedcba9876543210;
  const auto NullExecution = Invoke(nullptr, &Result);
  record(Records, Artifact, "null-execution", Before, State, NullExecution,
         Result, *Memory, Execution);
  EXPECT_EQ(NullExecution, 1u);
  EXPECT_EQ(Result, 0xfedcba9876543210u);
  const auto NullOutput = Invoke(&Execution, nullptr);
  record(Records, Artifact, "null-output", Before, State, NullOutput, Result,
         *Memory, Execution);
  EXPECT_EQ(NullOutput, 1u);
  EXPECT_EQ(rawState(State), rawState(Before));
}

void checkGenerated(bool Resets) {
#if defined(_WIN32)
  GTEST_SKIP() << "The standalone generated-library harness currently covers "
                  "ELF and Mach-O hosts";
#else
  const llvm::Triple Host(llvm::sys::getDefaultTargetTriple());
  if (Host.getArch() != llvm::Triple::x86_64 &&
      Host.getArch() != llvm::Triple::aarch64)
    GTEST_SKIP()
        << "The portable generated C interface requires an x64 or AArch64 host";
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Found = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(static_cast<bool>(Found))
      << "Clang is required to execute generated LLVM and C";
  const std::string Compiler = *Found;
#endif
  llvm::SmallString<256> Directory;
  const char *EvidenceRoot = std::getenv("NEVERD_X87_GENERATED_ARTIFACT_DIR");
  if (EvidenceRoot && *EvidenceRoot) {
    ASSERT_FALSE(llvm::sys::fs::create_directories(EvidenceRoot));
    llvm::SmallString<256> Prefix(EvidenceRoot);
    llvm::sys::path::append(Prefix, Resets ? "x87-resets" : "x87-generated");
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  } else {
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(
        Resets ? "x87-resets" : "x87-generated", Directory));
  }
  llvm::errs() << "Portable generated artifacts: " << Directory << '\n';
  const auto Remove = llvm::scope_exit([&] {
    if ((!EvidenceRoot || !*EvidenceRoot) && !::testing::Test::HasFailure())
      (void)llvm::sys::fs::remove_directories(Directory);
  });
  const auto Path = [&](llvm::StringRef Name) {
    llvm::SmallString<256> Result(Directory);
    llvm::sys::path::append(Result, Name);
    return Result.str().str();
  };
  std::error_code EC;
  llvm::raw_fd_ostream Records(Path("observations.jsonl"), EC);
  ASSERT_FALSE(EC);
  llvm::raw_fd_ostream Commands(Path("commands.jsonl"), EC);
  ASSERT_FALSE(EC);
  for (bool NoOpt : {false, true})
    for (unsigned Shape = 0; Shape != 3; ++Shape) {
      const auto Bytes = Resets       ? resetBytes(Shape)
                         : Shape == 0 ? portableX87LeafBytes()
                         : Shape == 1 ? portableX87BranchBytes()
                                      : portableX87LoopBytes();
      auto Source = liftPortableX87Fixture(Bytes, NoOpt);
      ASSERT_TRUE(static_cast<bool>(Source))
          << llvm::toString(Source.takeError());
      auto Prepared =
          preparePortableX87Function(*Source, Arch::X64, BinaryFormat::ELF);
      ASSERT_TRUE(static_cast<bool>(Prepared))
          << llvm::toString(Prepared.takeError());
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emitPortableX87(*Prepared, Context, Host.str());
      ASSERT_TRUE(static_cast<bool>(Module))
          << llvm::toString(Module.takeError());
      std::string LLVM;
      llvm::raw_string_ostream LLVMOut(LLVM);
      (*Module)->print(LLVMOut, nullptr);
      auto High = HighCEmitter().emitPortableX87(*Prepared, Host.str());
      ASSERT_TRUE(static_cast<bool>(High)) << llvm::toString(High.takeError());
      std::string LLVMC;
      llvm::raw_string_ostream COut(LLVMC);
      CEmitterOptions Options;
      Options.PreserveLLVMFunctionTypes = true;
      Options.EmitComments = false;
      ASSERT_TRUE(LLVMCEmitter().emit(**Module, COut, Options));
      const std::array Texts{LLVM, *High, LLVMC};
      for (unsigned Route = 0; Route != Texts.size(); ++Route)
        for (const char *Optimization : {"-O0", "-O2"}) {
          const std::string Name = std::string(NoOpt ? "noopt" : "default") +
                                   "-" + std::to_string(Shape) + "-" +
                                   std::to_string(Route) + Optimization;
          SCOPED_TRACE(Name);
          const auto SourcePath = Path(Name + (Route == 0 ? ".ll" : ".c"));
          const auto BinaryPath =
              Path(Name + (Host.isOSDarwin() ? ".dylib" : ".so"));
          const auto ErrorPath = Path(Name + ".stderr");
          {
            llvm::raw_fd_ostream Out(SourcePath, EC);
            ASSERT_FALSE(EC);
            Out << Texts[Route];
          }
          const std::string Runtime = NEVERD_X87_RUNTIME_PATH;
          const auto RPath =
              "-Wl,-rpath," + llvm::sys::path::parent_path(Runtime).str();
          std::vector<std::string> Arguments{
              Compiler, Host.isOSDarwin() ? "-dynamiclib" : "-shared", "-fPIC",
              Optimization, "-fno-strict-aliasing"};
          if (Route != 0)
            Arguments.push_back("-std=c11");
          Arguments.insert(Arguments.end(),
                           {SourcePath, Runtime, RPath, "-o", BinaryPath});
          llvm::SmallVector<llvm::StringRef> Views;
          llvm::json::Array Command;
          for (const auto &Argument : Arguments) {
            Views.push_back(Argument);
            Command.push_back(Argument);
          }
          Commands << llvm::json::Value(llvm::json::Object{
                          {"artifact", Name},
                          {"target", Host.str()},
                          {"source_bytes", llvm::toHex(Bytes)},
                          {"command", std::move(Command)}})
                   << '\n';
          Commands.flush();
          const std::optional<llvm::StringRef> Redirects[] = {
              std::nullopt, std::nullopt, ErrorPath};
          std::string Error;
          const auto Status = llvm::sys::ExecuteAndWait(
              Compiler, Views, std::nullopt, Redirects, 30, 0, &Error);
          auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
          ASSERT_EQ(Status, 0)
              << Error << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
              << SourcePath;
          Loaded Library{llvm::sys::DynamicLibrary::getLibrary(
              BinaryPath.c_str(), &Error)};
          ASSERT_TRUE(Library.Library.isValid()) << Error;
          auto *Symbol = Library.Library.getAddressOfSymbol(
              Prepared->name().str().c_str());
          ASSERT_NE(Symbol, nullptr);
          if (Resets)
            executeReset(Symbol, Shape, Bytes, Name, Records);
          else
            execute(Symbol, Shape, Bytes, Name, Records);
          Records.flush();
          ASSERT_FALSE(::testing::Test::HasFailure())
              << "Artifacts retained at " << Directory.str().str();
        }
    }
#endif
}

TEST(X87PortableGenerated, AllRoutesExecuteExactStateAndRefusalBoundaries) {
  checkGenerated(false);
}

TEST(X87PortableGenerated, AllRoutesExecuteFnopEmmsAndFemms) {
  checkGenerated(true);
}

} // namespace
