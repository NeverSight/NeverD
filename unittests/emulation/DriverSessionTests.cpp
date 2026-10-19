//===- DriverSessionTests.cpp - Original bounded-driver fixtures ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original bounded-driver fixtures.
///
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelReferenceImage.h"
#include "os/windows/kernel/WindowsKernelLayout.h"
#include "os/windows/process/WindowsProcessContext.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace neverd::emulation {
namespace {
std::filesystem::path fixture(const char *Name) {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + ".sys");
}

const DriverAPIEvent *findCall(const DriverResult &Result, const char *Name) {
  auto Found =
      std::find_if(Result.Calls.begin(), Result.Calls.end(),
                   [&](const auto &Call) { return Call.Name == Name; });
  return Found == Result.Calls.end() ? nullptr : &*Found;
}

TEST(DriverSession, DirectRejectsUnobservableScenarioOptionsBeforeLoading) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    SCOPED_TRACE(Mode);
    DriverOptions Options;
    Options.Contract = ExecutionContract::DirectDriverX64;
    Options.TraceMemoryWrites = false;
    const char *Expected = nullptr;
    if (Mode == 0) {
      Options.Scheduling = DriverScheduling{1, 1};
      Expected = "instruction-driven scheduling";
    } else if (Mode == 1) {
      Options.TraceMemoryWrites = true;
      Expected = "trace_memory_writes=false";
    } else {
      Options.CPUID = {{0, std::nullopt, {0, 0, 0, 0}}};
      Expected = "cannot apply scenario cpuid";
    }
    auto Result = emulateDriver("missing-direct-driver.sys", Options);
    ASSERT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find(Expected),
              std::string::npos);
  }
}

TEST(DriverSession, BugCheckBorrowedNameAllowsReturnAndDeregistration) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Unload : {false, true}) {
        DriverOptions O;
        O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
        O.Contract = Contract;
        O.Unload = Unload;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        auto R = emulateDriver(fixture("driver_bugcheck_borrowed_name"), O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
        EXPECT_EQ(R->UnloadCompleted, Unload);
        if (Unload) {
          EXPECT_TRUE(R->BugCheckCallbacks.empty());
        } else {
          ASSERT_EQ(R->BugCheckCallbacks.size(), 1u);
          EXPECT_TRUE(R->BugCheckCallbacks[0].ComponentStorageReleased);
          auto Parsed = llvm::json::parse(driverResultJSON(*R));
          ASSERT_TRUE(bool(Parsed));
          auto *Callbacks =
              Parsed->getAsObject()->getArray("bugcheck_callbacks");
          ASSERT_NE(Callbacks, nullptr);
          ASSERT_EQ(Callbacks->size(), 1u);
          EXPECT_EQ((*Callbacks)[0].getAsObject()->getBoolean(
                        "component_storage_released"),
                    true);
        }
      }
}

TEST(DriverSession, ThreadCIDsMatchGSAndSurviveHandleCloseAndExit) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    DriverOptions Options;
    Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    Options.Backend = ExecutionBackendKind::Unicorn;
    Options.Contract = Contract;
    Options.InstructionLimit = 10000;
    auto Result = emulateDriver(fixture("driver_thread_identity"), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;

    EXPECT_EQ(Result->NTStatus, 0u);
    const auto *Lookup = findCall(*Result, "PsLookupThreadByThreadId");
    ASSERT_NE(Lookup, nullptr);
    EXPECT_EQ(Lookup->Result, 0u);
    EXPECT_NE(findCall(*Result, "PsTerminateSystemThread"), nullptr);
    EXPECT_NE(findCall(*Result, "ObOpenObjectByPointer"), nullptr);
    EXPECT_NE(findCall(*Result, "ZwQueryInformationThread"), nullptr);
    EXPECT_NE(findCall(*Result, "KeWaitForSingleObject"), nullptr);
    EXPECT_NE(findCall(*Result, "PsGetCurrentThreadWin32Thread"), nullptr);
    EXPECT_NE(findCall(*Result, "PsGetThreadWin32Thread"), nullptr);
    EXPECT_NE(findCall(*Result, "PsSetThreadWin32Thread"), nullptr);
  }
}

TEST(DriverSession, FixedControlReadsPreserveGuestStateAndExactBudget) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    DriverOptions O;
    O.Contract = Contract;
    auto R = emulateDriver(fixture("driver_control_registers"), O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
    EXPECT_EQ(R->NTStatus, 0u);
    ASSERT_GT(R->Instructions, 1u);
    O.InstructionLimit = R->Instructions;
    auto Exact = emulateDriver(fixture("driver_control_registers"), O);
    ASSERT_TRUE(bool(Exact));
    EXPECT_EQ(Exact->Stop, DriverStopReason::Returned) << Exact->Diagnostic;
    O.InstructionLimit -= 1;
    auto Short = emulateDriver(fixture("driver_control_registers"), O);
    ASSERT_TRUE(bool(Short));
    EXPECT_EQ(Short->Stop, DriverStopReason::InstructionLimit);
    EXPECT_EQ(Short->Instructions, O.InstructionLimit);
    O.InstructionLimit = profile::DefaultInstructionLimit;
    auto Write = emulateDriver(fixture("driver_control_registers_write"), O);
    ASSERT_TRUE(bool(Write));
    EXPECT_EQ(Write->Stop, DriverStopReason::UnsupportedInstruction);
    EXPECT_NE(Write->Diagnostic.find("mov"), std::string::npos);
  }
}

TEST(DriverReport, LongCallsAndWritesPreserveOrderingNullsAndEscapedText) {
  DriverResult Result;
  Result.Stop = DriverStopReason::InstructionLimit;
  Result.Phase = "driver_entry";
  Result.Diagnostic = "budget \"quoted\"\nnext\\line";
  for (unsigned I = 0; I != 2000; ++I) {
    DriverAPIEvent Call;
    Call.PC = 0x1000 + I;
    Call.Phase = Result.Phase;
    Call.Name = "Routine";
    Call.Detail = "quote\" slash\\ newline\n";
    Call.Arguments = {0, 0xfffff80000000000ULL, I};
    if (I % 2 == 0)
      Call.Result = I;
    if (I % 3 == 0)
      Call.ReturnAddress = 0x2000 + I;
    Result.Calls.push_back(std::move(Call));
    Result.Writes.push_back(
        {Result.Phase, 0x3000 + I, 0x4000 + I, 8,
         I % 2 ? std::nullopt : std::optional<uint64_t>(I)});
  }
  auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  const auto *Root = JSON.getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getString("diagnostic"), Result.Diagnostic);
  const auto *Calls = Root->getArray("calls");
  const auto *Writes = Root->getArray("writes");
  ASSERT_NE(Calls, nullptr);
  ASSERT_NE(Writes, nullptr);
  ASSERT_EQ(Calls->size(), 2000u);
  ASSERT_EQ(Writes->size(), 2000u);
  for (unsigned I = 0; I != 2000; ++I) {
    const auto *Call = (*Calls)[I].getAsObject();
    const auto *Write = (*Writes)[I].getAsObject();
    ASSERT_NE(Call, nullptr);
    ASSERT_NE(Write, nullptr);
    auto Hex = [](uint64_t V) { return "0x" + llvm::utohexstr(V); };
    EXPECT_EQ(Call->getString("pc"), Hex(0x1000 + I));
    EXPECT_EQ(Call->getString("detail"), "quote\" slash\\ newline\n");
    ASSERT_EQ(Call->getArray("arguments")->size(), 3u);
    EXPECT_EQ((*Call->getArray("arguments"))[2].getAsString(), Hex(I));
    EXPECT_EQ(Call->getString("result").has_value(), I % 2 == 0);
    EXPECT_EQ(Call->getString("return_address").has_value(), I % 3 == 0);
    EXPECT_EQ(Write->getString("address"), Hex(0x4000 + I));
    EXPECT_EQ(Write->getString("value").has_value(), I % 2 == 0);
  }
  Result.Calls.clear();
  Result.Writes.clear();
  auto Empty = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  EXPECT_TRUE(Empty.getAsObject()->getArray("calls")->empty());
  EXPECT_TRUE(Empty.getAsObject()->getArray("writes")->empty());
}
TEST(DriverSession, RejectsPendingInitializationBeforeDeliveringScenario) {
  DriverOptions Options;
  Options.Requests.push_back({DriverRequestKind::Create});
  Options.Unload = true;
  auto Result = emulateDriver(fixture("pendingentry"), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_EQ(Result->NTStatus, 0x103u);
  EXPECT_TRUE(Result->Requests.empty());
  EXPECT_FALSE(Result->UnloadCompleted);
  EXPECT_NE(Result->Diagnostic.find("DriverEntry must return STATUS_SUCCESS"),
            std::string::npos);
}

TEST(DriverEmulation, ReturnsSuccessAndObservesDriverObjectWrites) {
  auto Result = emulateDriver(fixture("success"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus);
  EXPECT_EQ(*Result->NTStatus, 0u);
  EXPECT_GT(Result->Instructions, 0u);
  EXPECT_EQ(Result->ImageBase, 0x180000000ULL);
  EXPECT_NE(Result->DriverObject, 0u);
  EXPECT_GE(Result->DriverUnload, Result->ImageBase);
  EXPECT_GE(Result->MajorFunctions[0], Result->ImageBase);
  EXPECT_FALSE(Result->Writes.empty());
  EXPECT_TRUE(Result->KernelImageSHA256.empty());
  EXPECT_TRUE(Result->KernelPDBSHA256.empty());
}

TEST(DriverEmulation, PreservesFailingNTStatusAsNormalReturn) {
  auto Result = emulateDriver(fixture("failure"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus);
  EXPECT_EQ(*Result->NTStatus, 0xc0000001u);
}

TEST(DriverCPUID, ExplicitQueriesPreserveFlagsAndZeroExtendAllFourOutputs) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    DriverOptions Options;
    Options.Contract = Contract;
    Options.CPUID.push_back(
        {0x40000000, 3, {0xfedcba98, 0x80000001, 0x01234567, UINT32_MAX}});
    auto Result = emulateDriver(fixture("driver_cpuid"), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;

    EXPECT_EQ(Result->NTStatus, 0u);
    ASSERT_EQ(Result->Configuration.CPUID.size(), Options.CPUID.size());
    EXPECT_EQ(Result->Configuration.CPUID.front().Registers,
              Options.CPUID.front().Registers);
    Options.InstructionLimit = Result->Instructions;
    auto Exact = emulateDriver(fixture("driver_cpuid"), Options);
    ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
    EXPECT_EQ(Exact->Stop, DriverStopReason::Returned) << Exact->Diagnostic;
    EXPECT_EQ(Exact->Instructions, Options.InstructionLimit);
    --Options.InstructionLimit;
    auto Short = emulateDriver(fixture("driver_cpuid"), Options);
    ASSERT_TRUE(bool(Short)) << llvm::toString(Short.takeError());
    EXPECT_EQ(Short->Stop, DriverStopReason::InstructionLimit);
    EXPECT_EQ(Short->Instructions, Options.InstructionLimit);
    Options.InstructionLimit = DriverOptions{}.InstructionLimit;
    Options.CPUID.clear();
    auto Missing = emulateDriver(fixture("driver_cpuid"), Options);
    ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
    EXPECT_EQ(Missing->Stop, DriverStopReason::UnsupportedInstruction);
    EXPECT_FALSE(Missing->NTStatus);
    Options.CPUID.push_back({0x40000000, 2, {}});
    auto Other = emulateDriver(fixture("driver_cpuid"), Options);
    ASSERT_TRUE(bool(Other)) << llvm::toString(Other.takeError());
    EXPECT_EQ(Other->Stop, DriverStopReason::UnsupportedInstruction);
    EXPECT_EQ(Other->Diagnostic,
              "driver cpuid: undeclared leaf 0x40000000, subleaf 0x3");
    EXPECT_EQ(Other->PC, Missing->PC);
    EXPECT_EQ(Other->Instructions, Missing->Instructions);
    EXPECT_FALSE(Other->NTStatus);
  }
}

TEST(DriverProcessIdentity, CompiledClientKeepsOneReferenceUntilUnload) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    DriverOptions Options;
    Options.Contract = Contract;
    Options.Unload = true;
    auto Result = emulateDriver(fixture("driver_process_lookup"), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;

    EXPECT_EQ(Result->NTStatus, 0u);
    EXPECT_TRUE(Result->UnloadCompleted);
  }
}

TEST(DriverGuestPaging, CompiledClientWalksRecursiveTablesAndLockedMDLAliases) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    DriverOptions Options;
    Options.Contract = Contract;
    Options.PageTables = true;
    Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    Options.Unload = true;
    auto Result = emulateDriver(fixture("driver_paging"), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;

    EXPECT_EQ(Result->NTStatus, 0u);
    EXPECT_TRUE(Result->UnloadCompleted);
    auto *Translation = findCall(*Result, "MmGetVirtualForPhysical");
    ASSERT_NE(Translation, nullptr);
    EXPECT_TRUE(Translation->Result);
  }
}

TEST(DriverGuestPaging,
     NativeNonpagedRemappingSharesPFNsAndRestoresBorrowedVA) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    DriverOptions Options;
    Options.Contract = Contract;
    Options.PageTables = true;
    Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    Options.MDLMappingPolicy = DriverMDLMappingPolicy::Windows19041;
    Options.Unload = true;
    auto Result = emulateDriver(fixture("driver_paging_nonpaged"), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;

    EXPECT_EQ(Result->NTStatus, 0u);
    EXPECT_TRUE(Result->UnloadCompleted);
    Options.MDLMappingPolicy = DriverMDLMappingPolicy::Strict;
    auto Strict = emulateDriver(fixture("driver_paging_nonpaged"), Options);
    ASSERT_TRUE(bool(Strict)) << llvm::toString(Strict.takeError());
    EXPECT_EQ(Strict->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Strict->Diagnostic.find("additional system-space mapping"),
              std::string::npos);
  }
}

TEST(DriverGuestPaging,
     GuestPageTableWritesFaultAndDefaultCR3RemainsUnsupported) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    DriverOptions Options;
    Options.Contract = Contract;
    Options.PageTables = true;
    Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    auto Result = emulateDriver(fixture("driver_paging_write"), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::MemoryFault)
        << Result->Diagnostic;
    EXPECT_FALSE(Result->NTStatus);
    Options.PageTables = false;
    auto Disabled = emulateDriver(fixture("driver_paging"), Options);
    ASSERT_TRUE(bool(Disabled)) << llvm::toString(Disabled.takeError());
    EXPECT_EQ(Disabled->Stop, DriverStopReason::UnsupportedInstruction);
    EXPECT_FALSE(Disabled->NTStatus);
  }
}

TEST(DriverEmulation, UnsupportedImportStopsWithoutInventingSuccess) {
  auto Result = emulateDriver(fixture("unknown"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedAPI)
      << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
  EXPECT_NE(Result->Diagnostic.find("NeverDUnsupportedFixtureAPI"),
            std::string::npos);
}

TEST(DriverEmulation, InvalidGuestMemoryStopsWithObservations) {
  auto Result = emulateDriver(fixture("fault"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::MemoryFault) << Result->Diagnostic;
  EXPECT_GT(Result->Instructions, 0u);
  EXPECT_FALSE(Result->NTStatus);
  ASSERT_TRUE(Result->Fault);
  EXPECT_EQ(Result->Fault->Kind, "unmapped_memory");
  EXPECT_EQ(Result->Fault->Address, 0x12345000ULL);
  EXPECT_EQ(Result->Fault->Size, 8u);
  EXPECT_EQ(Result->Fault->Access, "write");
  EXPECT_EQ(Result->Fault->PC, Result->PC);
}

TEST(DriverEmulation, SectionWriteProtectionIsEnforced) {
  auto Result = emulateDriver(fixture("readonly"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::MemoryFault) << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, UnknownCPUEnvironmentDoesNotSilentlyExecuteCLI) {
  auto Result = emulateDriver(fixture("privileged"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedInstruction)
      << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, ReadingOpaqueDriverSectionStopsInsteadOfInventingState) {
  auto Result = emulateDriver(fixture("opaque"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, StackPivotIntoDriverObjectStopsBeforeAPIDispatch) {
  auto Result = emulateDriver(fixture("opaqueargument"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_NE(
      Result->Diagnostic.find("stack pointer exceeds the invocation stack"),
      std::string::npos);
  EXPECT_FALSE(Result->Fault);
  EXPECT_TRUE(Result->Calls.empty());
}

TEST(DriverEmulation, CurrentThreadReadUsesTheActiveExecutionIdentity) {
  auto Result = emulateDriver(fixture("gs"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0u);
}

TEST(DriverEmulation, KernelAPIPointersUseCheckedGuestMemory) {
  auto Result = emulateDriver(fixture("apifault"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::MemoryFault) << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, InfiniteLoopExhaustsExactInstructionBudget) {
  DriverOptions Options;
  Options.InstructionLimit = 50;
  auto Result = emulateDriver(fixture("loop"), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::InstructionLimit)
      << Result->Diagnostic;
  EXPECT_EQ(Result->Instructions, 50u);
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, ModelsStringsPoolAndDeviceCreation) {
  auto Result = emulateDriver(fixture("devices"));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus);
  EXPECT_EQ(*Result->NTStatus, 0u);
  ASSERT_EQ(Result->Devices.size(), 1u);
  EXPECT_EQ(Result->Devices[0].Name, "\\Device\\NeverDTest");
  EXPECT_EQ(Result->Devices[0].Type, 0x22u);
  EXPECT_NE(Result->Devices[0].Address, 0u);
  EXPECT_NE(Result->Devices[0].Extension, 0u);
  EXPECT_GE(Result->DriverUnload, Result->ImageBase);
  EXPECT_EQ(Result->MajorFunctions[0], Result->MajorFunctions[14]);
  EXPECT_GE(Result->MajorFunctions[14], Result->ImageBase);
  EXPECT_NE(findCall(*Result, "RtlInitUnicodeString"), nullptr);
  EXPECT_NE(findCall(*Result, "ExFreePoolWithTag"), nullptr);
  EXPECT_NE(findCall(*Result, "IoCreateSymbolicLink"), nullptr);
  const auto *Allocation = findCall(*Result, "ExAllocatePoolWithTag");
  ASSERT_NE(Allocation, nullptr);
  ASSERT_TRUE(Allocation->Result);
  EXPECT_NE(*Allocation->Result, 0u);
  EXPECT_TRUE(std::any_of(Result->Writes.begin(), Result->Writes.end(),
                          [&](const auto &Write) {
                            return Write.Address == *Allocation->Result &&
                                   Write.Value == 0x1122334455667788ULL;
                          }));
}

TEST(DriverEmulation, ObservationBudgetStopsExecution) {
  DriverOptions Options;
  Options.EventLimit = 1;
  auto Result = emulateDriver(fixture("devices"), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::EventLimit) << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, WallClockDeadlineStopsWithNoInstructionBudgetExhaustion) {
  DriverOptions Options;
  Options.InstructionLimit = UINT64_MAX;
  Options.EventLimit = 1000000;
  Options.TimeoutMilliseconds = 1;
  auto Result = emulateDriver(fixture("loop"), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Timeout) << Result->Diagnostic;
  EXPECT_LT(Result->Instructions, Options.InstructionLimit);
  EXPECT_FALSE(Result->NTStatus);
}

TEST(DriverEmulation, InvalidResourceLimitsAreRequestErrors) {
  for (unsigned Case = 0; Case < 7; ++Case) {
    SCOPED_TRACE(Case);
    DriverOptions Options;
    switch (Case) {
    case 0:
      Options.InstructionLimit = 0;
      break;
    case 1:
      Options.MemoryLimit = 0;
      break;
    case 2:
      Options.EventLimit = 0;
      break;
    case 3:
      Options.TimeoutMilliseconds = 0;
      break;
    case 4:
      Options.MemoryLimit = UINT64_MAX;
      break;
    case 5:
      Options.EventLimit = UINT64_MAX;
      break;
    case 6:
      Options.TimeoutMilliseconds = UINT64_MAX;
      break;
    }
    auto Result = emulateDriver(fixture("success"), Options);
    ASSERT_FALSE(static_cast<bool>(Result));
    EXPECT_FALSE(llvm::toString(Result.takeError()).empty());
  }
}

TEST(DriverEmulation, RegistryServiceNameCannotInjectAPath) {
  DriverOptions Options;
  Options.ServiceName = "..\\OtherService";
  auto Result = emulateDriver(fixture("success"), Options);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("service name"),
            std::string::npos);
}

TEST(DriverEmulation, MemoryBudgetIncludesStackAndKernelEnvironment) {
  DriverOptions Options;
  Options.MemoryLimit = 1024 * 1024;
  auto Result = emulateDriver(fixture("success"), Options);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("memory limit"),
            std::string::npos);
}

class DriverImageValidation : public ::testing::Test {
protected:
  std::vector<uint8_t> Bytes;
  std::unique_ptr<llvm::object::COFFObjectFile> Object;
  std::filesystem::path Directory;

  void SetUp() override {
    auto Buffer = llvm::MemoryBuffer::getFile(fixture("devices").string());
    ASSERT_TRUE(static_cast<bool>(Buffer));
    const auto Data = (*Buffer)->getBuffer();
    Bytes.assign(Data.bytes_begin(), Data.bytes_end());
    llvm::MemoryBufferRef Reference(
        llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                        Bytes.size()),
        "driver-image-test");
    auto Parsed = llvm::object::COFFObjectFile::create(Reference);
    ASSERT_TRUE(static_cast<bool>(Parsed))
        << llvm::toString(Parsed.takeError());
    Object = std::move(*Parsed);
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-driver-test", Temporary));
    Directory = Temporary.str().str();
  }

  void TearDown() override {
    std::error_code Ignored;
    std::filesystem::remove_all(Directory, Ignored);
  }

  size_t offset(const void *Address) const {
    return static_cast<const uint8_t *>(Address) - Bytes.data();
  }

  void put16(const void *Address, uint16_t Value) {
    llvm::support::endian::write16le(Bytes.data() + offset(Address), Value);
  }

  void put32(const void *Address, uint32_t Value) {
    llvm::support::endian::write32le(Bytes.data() + offset(Address), Value);
  }

  void put64(const void *Address, uint64_t Value) {
    llvm::support::endian::write64le(Bytes.data() + offset(Address), Value);
  }

  std::filesystem::path writeModified() {
    const auto Path = Directory / "invalid.sys";
    std::ofstream Output(Path, std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Output.close();
    EXPECT_TRUE(Output);
    return Path;
  }

  void replaceEntry(llvm::ArrayRef<uint8_t> Code) {
    uintptr_t Entry = 0;
    auto Error = Object->getRvaPtr(
        Object->getPE32PlusHeader()->AddressOfEntryPoint, Entry);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    const auto Offset = offset(reinterpret_cast<const void *>(Entry));
    ASSERT_LE(Offset + Code.size(), Bytes.size());
    std::copy(Code.begin(), Code.end(), Bytes.begin() + Offset);
  }

  void replaceImportProvider(llvm::StringRef Module) {
    const auto *Imports = Object->getDataDirectory(llvm::COFF::IMPORT_TABLE);
    ASSERT_NE(Imports, nullptr);
    uintptr_t DescriptorAddress = 0;
    auto Error =
        Object->getRvaPtr(Imports->RelativeVirtualAddress, DescriptorAddress);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    const auto *Descriptor = reinterpret_cast<
        const llvm::object::coff_import_directory_table_entry *>(
        DescriptorAddress);
    uintptr_t NameAddress = 0;
    Error = Object->getRvaPtr(Descriptor->NameRVA, NameAddress);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    const auto *Name = reinterpret_cast<const char *>(NameAddress);
    ASSERT_EQ(llvm::StringRef(Name), "ntoskrnl.exe");
    ASSERT_LE(Module.size(), llvm::StringRef(Name).size());
    const size_t NameOffset = offset(Name);
    std::copy(Module.begin(), Module.end(), Bytes.begin() + NameOffset);
    Bytes[NameOffset + Module.size()] = 0;
  }

  // Append a real DIR64 block to existing file padding, leaving all original
  // imports and code intact. The target is the first eight bytes of that data
  // section; at the preferred base the loader must not alter those bytes.
  struct RelocationFixture {
    size_t BlockOffset;
    uint64_t TargetAddress;
    std::array<uint8_t, 8> TargetBytes;
  };

  std::optional<RelocationFixture> addBaseRelocation() {
    const auto *Directory =
        Object->getDataDirectory(llvm::COFF::BASE_RELOCATION_TABLE);
    if (!Directory)
      return std::nullopt;
    for (const auto &Reference : Object->sections()) {
      const auto *Section = Object->getCOFFSection(Reference);
      const uint64_t Offset =
          (uint64_t(Section->VirtualSize) + 3) & ~uint64_t(3);
      if ((Section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) ||
          Section->VirtualSize < 8 || Offset + 12 > Section->SizeOfRawData)
        continue;
      const size_t Raw = Section->PointerToRawData;
      const size_t Block = Raw + Offset;
      const uint32_t RVA = Section->VirtualAddress;
      llvm::support::endian::write32le(Bytes.data() + Block, RVA & ~4095u);
      llvm::support::endian::write32le(Bytes.data() + Block + 4, 12);
      llvm::support::endian::write16le(
          Bytes.data() + Block + 8,
          (llvm::COFF::IMAGE_REL_BASED_DIR64 << 12) | (RVA & 4095));
      llvm::support::endian::write16le(Bytes.data() + Block + 10, 0);
      put32(&Section->VirtualSize, Offset + 12);
      put32(&Directory->RelativeVirtualAddress, RVA + Offset);
      put32(&Directory->Size, 12);
      put16(&Object->getCOFFHeader()->Characteristics,
            Object->getCharacteristics() &
                ~llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED);
      RelocationFixture Result{Block, Object->getImageBase() + RVA, {}};
      std::copy_n(Bytes.begin() + Raw, 8, Result.TargetBytes.begin());
      return Result;
    }
    return std::nullopt;
  }

  void rejects() {
    const auto Path = writeModified();
    auto Image = loadDriverImage(Path, 64 * 1024 * 1024);
    ASSERT_FALSE(static_cast<bool>(Image));
    const std::string Message = llvm::toString(Image.takeError());
    EXPECT_FALSE(Message.empty());
  }
};

TEST_F(DriverImageValidation, UnusedExternalImportsPermitActualEntryExecution) {
  const std::array<uint8_t, 3> Code = {0x31, 0xc0, 0xc3}; // xor eax,eax; ret
  replaceEntry(Code);
  replaceImportProvider("FLTMGR.SYS");
  const auto Path = writeModified();
  auto Image = loadDriverImage(Path, 64 * 1024 * 1024);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  ASSERT_FALSE(Image->Imports.empty());
  EXPECT_EQ(Image->Imports[0].Module, "FLTMGR.SYS");
  auto Result = emulateDriver(Path);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0u);
  EXPECT_EQ(Result->Instructions, 2u);
  EXPECT_TRUE(Result->Calls.empty());
}

TEST_F(DriverImageValidation,
       OriginalInstructionsReadAndUpdateLoadedModuleFlags) {
  // MOV RAX,[RCX+28h]; OR DWORD [RAX+68h],02000000h;
  // BTR DWORD [RAX+68h],25; MOV EAX,[RAX+68h]; RET.
  const uint8_t Code[] = {0x48, 0x8b, 0x41, 0x28, 0x81, 0x48, 0x68,
                          0x00, 0x00, 0x00, 0x02, 0x0f, 0xba, 0x70,
                          0x68, 0x19, 0x8b, 0x40, 0x68, 0xc3};
  replaceEntry(Code);
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  auto Result = emulateDriver(writeModified(), O);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0u);
  ASSERT_EQ(Result->Writes.size(), 2u);
  EXPECT_EQ(Result->Writes[0].Address, Result->Writes[1].Address);
  EXPECT_EQ(Result->Writes[0].Size, 4u);
  EXPECT_EQ(Result->Writes[1].Size, 4u);
}

TEST_F(DriverImageValidation,
       ExecutedExternalImportCannotBorrowSameNamedKernelImplementation) {
  replaceImportProvider("NDIS.SYS");
  auto Result = emulateDriver(writeModified());
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedAPI)
      << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
  EXPECT_GT(Result->Instructions, 0u);
  EXPECT_NE(Result->Diagnostic.find("ndis.sys!RtlInitUnicodeString"),
            std::string::npos);
  EXPECT_TRUE(Result->Calls.empty());
  EXPECT_TRUE(Result->Devices.empty());
}

TEST_F(DriverImageValidation, SyntheticImportReadReportsItsSymbolAndAddress) {
  auto Image = loadDriverImage(fixture("devices"), profile::DefaultMemoryLimit);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Import = std::find_if(
      Image->Imports.begin(), Image->Imports.end(),
      [](const auto &Entry) { return Entry.Name == "RtlInitUnicodeString"; });
  ASSERT_NE(Import, Image->Imports.end());
  // MOV RAX,[absolute IAT slot]; MOV RAX,[RAX+3]; RET. The interior read must
  // name the owning synthetic import rather than inventing function bytes.
  uint8_t Code[] = {0x48, 0xa1, 0,    0,    0,    0,    0,   0,
                    0,    0,    0x48, 0x8b, 0x40, 0x03, 0xc3};
  llvm::support::endian::write64le(Code + 2, Import->Slot);
  replaceEntry(Code);
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    DriverOptions Options;
    Options.Contract = Contract;
    auto Result = emulateDriver(writeModified(), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedAPI);
    EXPECT_NE(Result->Diagnostic.find("ntoskrnl.exe!RtlInitUnicodeString"),
              std::string::npos);
    EXPECT_NE(Result->Diagnostic.find("at 0x"), std::string::npos);
    EXPECT_NE(Result->Diagnostic.find("(8 bytes)"), std::string::npos);
    EXPECT_TRUE(Result->Calls.empty());
    EXPECT_FALSE(Result->NTStatus);
  }
}

TEST_F(DriverImageValidation, CurrentThreadComparisonsPreserveFlagsAndBudget) {
  // MOV rax,gs:[0x188]; CMP rax,gs:[0x188]; SETNE al; MOVZX eax,al; RET.
  const std::array<uint8_t, 25> Code = {
      0x65, 0x48, 0x8b, 0x04, 0x25, 0x88, 0x01, 0x00, 0x00,
      0x65, 0x48, 0x3b, 0x04, 0x25, 0x88, 0x01, 0x00, 0x00,
      0x0f, 0x95, 0xc0, 0x0f, 0xb6, 0xc0, 0xc3};
  replaceEntry(Code);
  const auto Path = writeModified();
  auto Result = emulateDriver(Path);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0u);
  EXPECT_EQ(Result->Instructions, 5u);

  DriverOptions Options;
  Options.InstructionLimit = 1;
  Result = emulateDriver(Path, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::InstructionLimit);
  EXPECT_EQ(Result->Instructions, 1u);
}

TEST_F(DriverImageValidation, PrivateProcessorViewRejectsOrdinaryAddressReads) {
  // First admit the GS field, then attempt an ordinary absolute address read.
  std::array<uint8_t, 20> Code = {0x65, 0x48, 0x8b, 0x04, 0x25, 0x88, 0x01,
                                  0x00, 0x00, 0x48, 0xa1, 0,    0,    0,
                                  0,    0,    0,    0,    0,    0xc3};
  llvm::support::endian::write64le(Code.data() + 11,
                                   profile::ProcessorEnvironmentBase +
                                       windows::GSCurrentThreadOffset);
  replaceEntry(Code);
  auto Result = emulateDriver(writeModified());
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_NE(Result->Diagnostic.find("processor view"), std::string::npos);
  EXPECT_EQ(Result->Instructions, 2u);
  EXPECT_FALSE(Result->NTStatus);
}

TEST_F(DriverImageValidation, UnknownProcessorFieldRemainsUnsupported) {
  // The neighboring field is not part of the declared current-thread view.
  std::array<uint8_t, 10> Code = {0x65, 0x48, 0x8b, 0x04, 0x25,
                                  0,    0,    0,    0,    0xc3};
  llvm::support::endian::write32le(
      Code.data() + 5, windows::GSCurrentThreadOffset + profile::PointerSize);
  replaceEntry(Code);
  auto Result = emulateDriver(writeModified());
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedInstruction);
  EXPECT_EQ(Result->Instructions, 0u);
  EXPECT_FALSE(Result->NTStatus);
}

TEST_F(DriverImageValidation,
       UndefinedDoubleShiftRetainsItsReasonBeforeScenarioDispatch) {
  // shrd ax, dx, 17 has an undefined destination and flags, followed by ret.
  constexpr uint8_t Code[] = {0x66, 0x0f, 0xac, 0xd0, 0x11, 0xc3};
  replaceEntry(Code);
  DriverOptions Options;
  Options.Backend = ExecutionBackendKind::Unicorn;
  Options.Contract = ExecutionContract::CheckedX64;
  Options.Requests.push_back({DriverRequestKind::Create});
  Options.Unload = true;
  auto Result = emulateDriver(writeModified(), Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedInstruction);
  EXPECT_EQ(Result->Instructions, 1u);
  EXPECT_FALSE(Result->NTStatus);
  EXPECT_TRUE(Result->Calls.empty());
  EXPECT_TRUE(Result->Requests.empty());
  EXPECT_FALSE(Result->UnloadCompleted);
  EXPECT_NE(Result->Diagnostic.find("double-shift count 17"),
            std::string::npos);
  EXPECT_NE(Result->Diagnostic.find("16-bit"), std::string::npos);
  EXPECT_NE(Result->Diagnostic.find("undefined"), std::string::npos);
  ASSERT_TRUE(Result->Fault);
  EXPECT_EQ(Result->Fault->Kind, "invalid_instruction");
  EXPECT_EQ(Result->Fault->PC, Result->PC);
}

TEST_F(DriverImageValidation, ExecuteOnlyCodeDoesNotRequireDataReadPermission) {
  bool Changed = false;
  for (const auto &Reference : Object->sections()) {
    const auto *Section = Object->getCOFFSection(Reference);
    if (Section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) {
      put32(&Section->Characteristics,
            Section->Characteristics & ~(llvm::COFF::IMAGE_SCN_MEM_READ |
                                         llvm::COFF::IMAGE_SCN_MEM_WRITE));
      Changed = true;
    }
  }
  ASSERT_TRUE(Changed);
  auto Result = emulateDriver(writeModified());
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus);
  EXPECT_EQ(*Result->NTStatus, 0u);
  EXPECT_EQ(Result->Devices.size(), 1u);
}

TEST_F(DriverImageValidation, RejectsNonNativeSubsystem) {
  put16(&Object->getPE32PlusHeader()->Subsystem,
        llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI);
  rejects();
}

TEST_F(DriverImageValidation, RejectsTruncatedRawSection) {
  Bytes.pop_back();
  rejects();
}

TEST_F(DriverImageValidation, RejectsEntryInHeaders) {
  put32(&Object->getPE32PlusHeader()->AddressOfEntryPoint, 0x100);
  rejects();
}

TEST_F(DriverImageValidation, RejectsOverlappingVirtualSections) {
  auto Section = Object->section_begin();
  const auto FirstRVA = Object->getCOFFSection(*Section)->VirtualAddress;
  ++Section;
  ASSERT_NE(Section, Object->section_end());
  put32(&Object->getCOFFSection(*Section)->VirtualAddress, FirstRVA);
  rejects();
}

TEST_F(DriverImageValidation, RejectsReservedKernelModelAddressRange) {
  put64(&Object->getPE32PlusHeader()->ImageBase, 0x70000000);
  rejects();
}

TEST_F(DriverImageValidation, RejectsReservedProcessorViewAddressRange) {
  put64(&Object->getPE32PlusHeader()->ImageBase,
        profile::ProcessorEnvironmentBase);
  rejects();
}

TEST_F(DriverImageValidation, PreferredBasePreservesDIR64TargetBytes) {
  const auto Relocation = addBaseRelocation();
  ASSERT_TRUE(Relocation);
  auto Image = loadDriverImage(writeModified(), 64 * 1024 * 1024);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (const auto &Region : Image->Regions) {
    if (Region.Address != Relocation->TargetAddress)
      continue;
    ASSERT_GE(Region.Bytes.size(), 8u);
    EXPECT_TRUE(std::equal(Relocation->TargetBytes.begin(),
                           Relocation->TargetBytes.end(),
                           Region.Bytes.begin()));
    return;
  }
  FAIL() << "relocation target region was not mapped";
}

TEST_F(DriverImageValidation, RejectsOversizedBaseRelocationBlock) {
  const auto Relocation = addBaseRelocation();
  ASSERT_TRUE(Relocation);
  llvm::support::endian::write32le(Bytes.data() + Relocation->BlockOffset + 4,
                                   0x10000000);
  rejects();
}

TEST_F(DriverImageValidation, RejectsNonX64BaseRelocationType) {
  const auto Relocation = addBaseRelocation();
  ASSERT_TRUE(Relocation);
  llvm::support::endian::write16le(Bytes.data() + Relocation->BlockOffset + 8,
                                   llvm::COFF::IMAGE_REL_BASED_HIGHLOW << 12);
  rejects();
}

TEST_F(DriverImageValidation, RejectsImportDirectoryWithoutNullDescriptor) {
  const auto *Directory = Object->getDataDirectory(llvm::COFF::IMPORT_TABLE);
  ASSERT_NE(Directory, nullptr);
  ASSERT_NE(uint32_t(Directory->RelativeVirtualAddress), 0u);
  put32(&Directory->Size,
        sizeof(llvm::object::coff_import_directory_table_entry));
  rejects();
}

TEST_F(DriverImageValidation, RejectsUnmappedImportLookupWithoutIteratingIt) {
  const auto *Directory = Object->getDataDirectory(llvm::COFF::IMPORT_TABLE);
  ASSERT_NE(Directory, nullptr);
  uintptr_t Address = 0;
  auto Error = Object->getRvaPtr(Directory->RelativeVirtualAddress, Address);
  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  const auto *Descriptor =
      reinterpret_cast<const llvm::object::coff_import_directory_table_entry *>(
          Address);
  put32(&Descriptor->ImportLookupTableRVA, 0xfffff000);
  rejects();
}

TEST_F(DriverImageValidation, RejectsImportProviderPaths) {
  const auto *Directory = Object->getDataDirectory(llvm::COFF::IMPORT_TABLE);
  ASSERT_NE(Directory, nullptr);
  uintptr_t Address = 0;
  auto Error = Object->getRvaPtr(Directory->RelativeVirtualAddress, Address);
  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  const auto *Descriptor =
      reinterpret_cast<const llvm::object::coff_import_directory_table_entry *>(
          Address);
  Error = Object->getRvaPtr(Descriptor->NameRVA, Address);
  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  ASSERT_EQ(std::string(reinterpret_cast<const char *>(Address)),
            "ntoskrnl.exe");
  const char OtherProvider[] = "../hal.dll";
  std::copy_n(OtherProvider, sizeof(OtherProvider),
              Bytes.data() + offset(reinterpret_cast<const void *>(Address)));
  rejects();
}

TEST_F(DriverImageValidation, ReservesEveryKernelImportModuleSpan) {
  const auto *Header = Object->getPE32PlusHeader();
  for (unsigned I = 0; I < profile::KernelModuleCount; ++I) {
    const uint64_t Base =
        profile::KernelModuleBase + I * profile::KernelModuleStride;
    for (const uint64_t Address :
         {Base, Base + profile::KernelModuleStride - 0x10000}) {
      SCOPED_TRACE(Address);
      put64(&Header->ImageBase, Address);
      auto Image = loadDriverImage(writeModified(), 64 * 1024 * 1024);
      ASSERT_FALSE(bool(Image));
      EXPECT_NE(llvm::toString(Image.takeError()).find("reserved"),
                std::string::npos);
    }
  }
  put64(&Header->ImageBase,
        profile::KernelModuleBase +
            profile::KernelModuleCount * profile::KernelModuleStride);
  auto Image = loadDriverImage(writeModified(), 64 * 1024 * 1024);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
}

class DriverImportProvider
    : public DriverImageValidation,
      public testing::WithParamInterface<ExecutionBackendKind> {
protected:
  void replaceProvider(llvm::StringRef Module) {
    const auto *Table = Object->getDataDirectory(llvm::COFF::IMPORT_TABLE);
    ASSERT_NE(Table, nullptr);
    uintptr_t Address = 0;
    auto Error = Object->getRvaPtr(Table->RelativeVirtualAddress, Address);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    const auto *Descriptor = reinterpret_cast<
        const llvm::object::coff_import_directory_table_entry *>(Address);
    Error = Object->getRvaPtr(Descriptor->NameRVA, Address);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    ASSERT_EQ(std::string(reinterpret_cast<const char *>(Address)),
              "ntoskrnl.exe");
    const auto At = offset(reinterpret_cast<const void *>(Address));
    ASSERT_LT(Module.size(), sizeof("ntoskrnl.exe"));
    std::fill_n(Bytes.begin() + At, sizeof("ntoskrnl.exe"), 0);
    std::copy(Module.begin(), Module.end(), Bytes.begin() + At);
  }

  void requireBackend() {
    auto Backend = createExecutionBackend(
        GetParam(), ExecutionContract::CheckedX64, 64 * 1024 * 1024);
    if (!Backend) {
      auto Error = Backend.takeError();
      const bool Unavailable = Error.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(Error));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
  }
};

TEST_P(DriverImportProvider, UnusedImportsDoNotRequireInventedImplementations) {
  requireBackend();
  if (HasFatalFailure() || IsSkipped())
    return;
  const auto Original = Bytes;
  for (const char *Module : {"FLTMGR.SYS", "NDIS.SYS", "NETIO.SYS"}) {
    SCOPED_TRACE(Module);
    std::copy(Original.begin(), Original.end(), Bytes.begin());
    replaceProvider(Module);
    ASSERT_FALSE(HasFatalFailure());
    // Independently authored DriverEntry: return STATUS_SUCCESS without calls.
    replaceEntry({0x31, 0xc0, 0xc3});
    DriverOptions Options;
    Options.Backend = GetParam();
    Options.Contract = ExecutionContract::CheckedX64;
    auto Result = emulateDriver(writeModified(), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;

    EXPECT_EQ(Result->NTStatus, 0u);
    EXPECT_EQ(Result->Instructions, 2u);
    EXPECT_TRUE(Result->Calls.empty());
  }
}

TEST_P(DriverImportProvider, ExecutedImportsRetainProviderQualifiedRefusal) {
  requireBackend();
  if (HasFatalFailure() || IsSkipped())
    return;
  const auto Original = Bytes;
  for (const char *Module : {"FLTMGR.SYS", "NDIS.SYS", "NETIO.SYS"}) {
    SCOPED_TRACE(Module);
    std::copy(Original.begin(), Original.end(), Bytes.begin());
    replaceProvider(Module);
    ASSERT_FALSE(HasFatalFailure());
    DriverOptions Options;
    Options.Backend = GetParam();
    Options.Contract = ExecutionContract::CheckedX64;
    Options.Requests.push_back({DriverRequestKind::Create});
    Options.Unload = true;
    auto Result = emulateDriver(writeModified(), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedAPI)
        << Result->Diagnostic;
    EXPECT_EQ(Result->Diagnostic,
              "unsupported import: " + llvm::StringRef(Module).lower() +
                  "!RtlInitUnicodeString");
    EXPECT_GT(Result->Instructions, 0u);
    EXPECT_FALSE(Result->NTStatus);
    EXPECT_TRUE(Result->Calls.empty());
    EXPECT_TRUE(Result->Requests.empty());
    EXPECT_FALSE(Result->UnloadCompleted);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Backends, DriverImportProvider,
    testing::Values(ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                    ExecutionBackendKind::WHP, ExecutionBackendKind::HVF),
    [](const testing::TestParamInfo<ExecutionBackendKind> &Info) {
      return std::string(executionBackendName(Info.param));
    });

TEST_F(DriverImageValidation, RejectsInvalidLoadConfigurationSize) {
  const auto *Directory =
      Object->getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
  ASSERT_NE(Directory, nullptr);
  for (const auto &Reference : Object->sections()) {
    const auto *Section = Object->getCOFFSection(Reference);
    if (Section->SizeOfRawData >= 8 &&
        !(Section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE)) {
      // Size zero avoids requiring any cookie layout during LLVM's parse. The
      // execution profile still rejects presence of the directory itself.
      llvm::support::endian::write32le(Bytes.data() + Section->PointerToRawData,
                                       0);
      put32(&Directory->RelativeVirtualAddress, Section->VirtualAddress);
      put32(&Directory->Size, 8);
      rejects();
      return;
    }
  }
  FAIL() << "fixture needs a file-backed nonexecutable section";
}

TEST(DriverImage, MapsVirtualZeroFillAndOriginalPermissions) {
  auto Image = loadDriverImage(fixture("success"), 64 * 1024 * 1024);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Base, 0x180000000ULL);
  ASSERT_FALSE(Image->Regions.empty());
  EXPECT_EQ(Image->Regions.front().Address, Image->Base);
  EXPECT_EQ(Image->Regions.front().Permissions, 1u);
  bool HasExecutable = false;
  bool HasWritableZeroFill = false;
  for (const auto &Region : Image->Regions) {
    EXPECT_EQ(Region.Address % 4096, 0u);
    EXPECT_EQ(Region.Bytes.size() % 4096, 0u);
    HasExecutable |= (Region.Permissions & 4) != 0;
    if ((Region.Permissions & 2) &&
        std::all_of(Region.Bytes.begin(), Region.Bytes.end(),
                    [](uint8_t Byte) { return Byte == 0; }))
      HasWritableZeroFill = true;
  }
  EXPECT_TRUE(HasExecutable);
  EXPECT_TRUE(HasWritableZeroFill);
}
TEST(DriverKernelContext, CapturesSupervisorCallerWithoutChangingCPU) {
  auto Backend = UnicornBackend::create(4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = **Backend;
  constexpr uint64_t Base = 0xfffff80010000000ULL;
  constexpr uint64_t Destination = Base + 0x100;
  constexpr uint64_t Stack = Base + 0x1008;
  constexpr uint64_t ReturnPC = 0xfffff80020001234ULL;
  llvm::cantFail(CPU.map(Base, 8192, Read | Write));
  const std::vector<uint8_t> Initial(1232, 0xa5);
  llvm::cantFail(CPU.write(Destination, Initial));
  llvm::cantFail(CPU.writeInteger(Stack, ReturnPC, 8));
  llvm::cantFail(CPU.writeRegister(CPURegister::X64SP, {Stack, 0}));
  llvm::cantFail(
      CPU.writeRegister(CPURegister::X64AX, {0x123456789abcdef0ULL, 0}));
  llvm::cantFail(CPU.writeRegister(CPURegister::X64PC, {0xabcdef, 0}));
  auto User = windows_process::captureCallerContext(CPU, Destination);
  ASSERT_TRUE(bool(User));
  llvm::consumeError(std::move(User));
  std::vector<uint8_t> Bytes(1232);
  llvm::cantFail(CPU.read(Destination, Bytes));
  EXPECT_EQ(Bytes, Initial);
  auto Kernel = windows_process::captureCallerContext(CPU, Destination, false);
  ASSERT_FALSE(bool(Kernel)) << llvm::toString(std::move(Kernel));
  llvm::cantFail(CPU.read(Destination, Bytes));
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 120),
            0x123456789abcdef0ULL);
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 152), Stack + 8);
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 248), ReturnPC);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 48), 0x10000fu);
  EXPECT_TRUE(std::equal(Bytes.begin(), Bytes.begin() + 48, Initial.begin()));
  EXPECT_TRUE(
      std::equal(Bytes.begin() + 672, Bytes.end(), Initial.begin() + 672));
  EXPECT_EQ(llvm::cantFail(CPU.readRegister(CPURegister::X64SP))[0], Stack);
  EXPECT_EQ(llvm::cantFail(CPU.readRegister(CPURegister::X64PC))[0], 0xabcdefu);
  auto Misaligned =
      windows_process::captureCallerContext(CPU, Destination + 1, false);
  ASSERT_TRUE(bool(Misaligned));
  llvm::consumeError(std::move(Misaligned));
}

TEST(DriverPersistentDump, OriginalCompiledClientCapturesMetadataAndFreesPool) {
  DriverOptions Options;
  Options.Contract = ExecutionContract::CheckedX64;
  Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  Options.InstructionLimit = 100000;
  auto Result = emulateDriver(fixture("driver_persistent_dump"), Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0u);
  auto *Capture = findCall(*Result, "KeCapturePersistentThreadState");
  ASSERT_NE(Capture, nullptr);
  EXPECT_EQ(Capture->Result, 0x40000u);
  ASSERT_EQ(Capture->Arguments.size(), 8u);
  EXPECT_EQ(Capture->Arguments[3], 0x1122334455667788u);
  ASSERT_TRUE(Capture->ReturnAddress);
  EXPECT_GT(*Capture->ReturnAddress, Result->ImageBase);
  EXPECT_NE(findCall(*Result, "ExFreePoolWithTag"), nullptr);
}

TEST(DriverPersistentDump, OriginalCompiledClientCannotConsumeUnknownKDBytes) {
  DriverOptions Options;
  Options.Contract = ExecutionContract::CheckedX64;
  Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  Options.InstructionLimit = 100000;
  auto Result =
      emulateDriver(fixture("driver_persistent_dump_unknown"), Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_FALSE(Result->NTStatus);
  EXPECT_NE(Result->Diagnostic.find("offset 0x2080"), std::string::npos);
  auto *Capture = findCall(*Result, "KeCapturePersistentThreadState");
  ASSERT_NE(Capture, nullptr);
  EXPECT_EQ(Capture->Result, 0x40000u);
  EXPECT_EQ(findCall(*Result, "ExFreePoolWithTag"), nullptr);
}

TEST(DriverPersistentDump, CheckedExecutionUsesTheSameCaptureBoundary) {
  for (bool Unknown : {false, true}) {
    DriverOptions Options;
    Options.Contract = ExecutionContract::CheckedX64;
    Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    Options.InstructionLimit = 100000;
    auto Result =
        emulateDriver(fixture(Unknown ? "driver_persistent_dump_unknown"
                                      : "driver_persistent_dump"),
                      Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, Unknown ? DriverStopReason::ModelError
                                    : DriverStopReason::Returned)
        << Result->Diagnostic;
    if (Unknown)
      EXPECT_NE(Result->Diagnostic.find("offset 0x2080"), std::string::npos);
    else

      EXPECT_EQ(Result->NTStatus, 0u);
  }
}

TEST(DriverKernelReference, OriginalCodeAndHeadersAreReadableButNeverWritable) {
  auto Reference = KernelReferenceImage::load(
      fixture("driver_kernel_reference"), 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
  EXPECT_EQ(Reference->digest().size(), 64u);
  ASSERT_NE(Reference->exports().at("ReferenceAccessor"), 0u);
  EXPECT_EQ(Reference->exports().at("ReferenceData"), 0u);
  auto Backend = UnicornBackend::create(4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &Memory = **Backend;
  llvm::cantFail(Reference->map(Memory));
  EXPECT_EQ(llvm::cantFail(Memory.readInteger(Reference->base(), 2)), 0x5a4du);
  const uint64_t Accessor = Reference->exports().at("ReferenceAccessor");
  EXPECT_EQ(llvm::cantFail(Memory.readInteger(Accessor + 3, 4)), 0x80u);
  EXPECT_FALSE(llvm::cantFail(Memory.canAccess(Accessor, 1, Write)));
  EXPECT_TRUE(llvm::cantFail(Memory.canAccess(Accessor, 1, Read | Execute)));
  auto TooSmall =
      KernelReferenceImage::load(fixture("driver_kernel_reference"), 1);
  ASSERT_FALSE(bool(TooSmall));
  llvm::consumeError(TooSmall.takeError());
}

TEST_F(DriverImageValidation,
       ReferenceCallsUseModelsAndInteriorCodeDoesNotExecute) {
  auto Reference = KernelReferenceImage::load(
      fixture("driver_kernel_reference"), 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
  const auto Target = Reference->exports().at("KeGetCurrentIrql");
  ASSERT_NE(Target, 0u);
  DriverOptions Options;
  Options.KernelImage = fixture("driver_kernel_reference");
  auto Original = loadDriverImage(fixture("devices"), 64 * 1024 * 1024);
  ASSERT_TRUE(bool(Original)) << llvm::toString(Original.takeError());
  for (const auto &Import : Original->Imports)
    Options.KernelExports[Import.Name] = true;
  for (uint64_t Offset : {0u, 1u}) {
    std::vector<uint8_t> Code = {0x48, 0x83, 0xec, 0x28, 0x48, 0xb8};
    const uint64_t Address = Target + Offset;
    for (unsigned I = 0; I < 8; ++I)
      Code.push_back(uint8_t(Address >> (I * 8)));
    const uint8_t Tail[] = {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    Code.insert(Code.end(), std::begin(Tail), std::end(Tail));
    replaceEntry(Code);
    auto Result = emulateDriver(writeModified(), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->KernelImageSHA256, Reference->digest());
    if (!Offset) {
      EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;

      EXPECT_EQ(Result->NTStatus, 0u);
      ASSERT_EQ(Result->Calls.size(), 1u);
      EXPECT_EQ(Result->Calls[0].Name, "KeGetCurrentIrql");
      EXPECT_EQ(Result->Calls[0].PC, Target);
      EXPECT_EQ(Result->Calls[0].ReturnAddress, Result->Entry + 16);
    } else {
      EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedAPI);
      EXPECT_NE(Result->Diagnostic.find("inside reference kernel"),
                std::string::npos);
      EXPECT_TRUE(Result->Calls.empty());
    }
  }
}

TEST_F(DriverImageValidation, ReferenceRejectsSectionsOutsideDeclaredImage) {
  auto Buffer =
      llvm::MemoryBuffer::getFile(fixture("driver_kernel_reference").string());
  ASSERT_TRUE(bool(Buffer));
  const auto Data = (*Buffer)->getBuffer();
  Bytes.assign(Data.bytes_begin(), Data.bytes_end());
  auto Parsed = llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                      Bytes.size()),
      "kernel-reference-test"));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  Object = std::move(*Parsed);
  auto *Header = Object->getPE32PlusHeader();
  ASSERT_NE(Header, nullptr);
  const auto Original = uint32_t(Header->SizeOfImage);
  for (uint32_t Size : {0u, 0x1000u, Original + 1, 0x40000000u}) {
    put32(&Header->SizeOfImage, Size);
    auto Reference =
        KernelReferenceImage::load(writeModified(), 4 * 1024 * 1024);
    ASSERT_FALSE(bool(Reference)) << Size;
    llvm::consumeError(Reference.takeError());
  }
  put32(&Header->SizeOfImage, Original);
  auto Reference = KernelReferenceImage::load(writeModified(), 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
}

} // namespace
} // namespace neverd::emulation
