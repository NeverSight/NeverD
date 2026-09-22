//===- KernelAPIIRQLTests.cpp - Kernel API caller IRQL tests -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Complete routine inventory and Windows x64 caller-IRQL contract coverage.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/KernelAPIIRQL.h"

#include <set>
#include <string>

namespace neverd::emulation {
namespace {

void expectLimit(llvm::StringRef Name, uint8_t Expected) {
  auto Maximum = maximumKernelIRQL(Name);
  if (!Maximum) {
    ADD_FAILURE() << llvm::toString(Maximum.takeError());
    return;
  }
  EXPECT_EQ(*Maximum, Expected) << Name.str();
}

TEST(DriverKernelAPIIRQL, CoversExactSupportedInventoryWithoutDuplicates) {
  const llvm::StringLiteral Supported[] = {
#define NEVERD_KERNEL_API(Name, Arity, Availability) #Name,
#include "windows/KernelAPIs.def"
#undef NEVERD_KERNEL_API
  };
  const llvm::StringLiteral Catalog[] = {
#define NEVERD_KERNEL_IRQL_API(Name, Maximum) #Name,
#include "windows/KernelAPIIRQL.def"
#undef NEVERD_KERNEL_IRQL_API
  };
  std::set<std::string> SupportedNames;
  for (auto Name : Supported) {
    EXPECT_TRUE(SupportedNames.insert(Name.str()).second) << Name.str();
    auto Maximum = maximumKernelIRQL(Name);
    if (!Maximum)
      ADD_FAILURE() << llvm::toString(Maximum.takeError());
    else
      EXPECT_LE(*Maximum, 15) << Name.str();
  }
  std::set<std::string> CatalogNames;
  for (auto Name : Catalog)
    EXPECT_TRUE(CatalogNames.insert(Name.str()).second) << Name.str();
  EXPECT_EQ(CatalogNames, SupportedNames);
}

TEST(DriverKernelAPIIRQL, EveryRegistryOperationRequiresPassiveLevel) {
#define NEVERD_KERNEL_REGISTRY_API(Name, Arity) expectLimit(#Name, 0);
#include "windows/KernelRegistryAPIs.def"
#undef NEVERD_KERNEL_REGISTRY_API
}

TEST(DriverKernelAPIIRQL, UnicodeCopyInitAndComparisonHaveDifferentCeilings) {
  expectLimit("RtlCopyUnicodeString", 15);
  expectLimit("RtlInitUnicodeString", 2);
  expectLimit("RtlCompareUnicodeString", 0);
  expectLimit("RtlEqualUnicodeString", 0);
}

TEST(DriverKernelAPIIRQL, DeviceCreationDeletionAndSymbolicLinksDiffer) {
  expectLimit("IoCreateDevice", 1);
  expectLimit("IoDeleteDevice", 1);
  expectLimit("IoCreateSymbolicLink", 0);
  expectLimit("IoDeleteSymbolicLink", 0);
  expectLimit("MmGetSystemRoutineAddress", 0);
}

TEST(DriverKernelAPIIRQL,
     WaitPollingCeilingDoesNotAuthorizeBlockingAtDispatch) {
  expectLimit("KeWaitForSingleObject", 2);
  expectLimit("KeDelayExecutionThread", 1);
}

TEST(DriverKernelAPIIRQL, NonpagedPoolMdlAndWorkOperationsPermitDispatch) {
  const llvm::StringLiteral Names[] = {"ExAllocatePoolWithTag",
                                       "ExAllocatePool2",
                                       "ExFreePoolWithTag",
                                       "ExFreePool",
                                       "IoAllocateMdl",
                                       "IoFreeMdl",
                                       "MmBuildMdlForNonPagedPool",
                                       "MmMapLockedPagesSpecifyCache",
                                       "MmGetSystemAddressForMdlSafe",
                                       "MmUnmapLockedPages",
                                       "IofCompleteRequest",
                                       "IoCompleteRequest",
                                       "IoAllocateWorkItem",
                                       "IoQueueWorkItem",
                                       "IoFreeWorkItem"};
  for (auto Name : Names)
    expectLimit(Name, 2);
}

TEST(DriverKernelAPIIRQL, DebugOutputStopsBelowClockAndIpiLevels) {
  expectLimit("DbgPrint", 12);
  expectLimit("DbgPrintEx", 12);
}

TEST(DriverKernelAPIIRQL,
     ResidentByteOperationsAndIrpHelpersPermitAnyX64Level) {
  const llvm::StringLiteral Names[] = {
      "memcpy",           "memmove",
      "memset",           "memcmp",
      "RtlCopyMemory",    "RtlMoveMemory",
      "RtlFillMemory",    "RtlCompareMemory",
      "RtlZeroMemory",    "IoGetCurrentIrpStackLocation",
      "IoMarkIrpPending", "KeGetCurrentIrql"};
  for (auto Name : Names)
    expectLimit(Name, 15);
}

TEST(DriverKernelAPIIRQL, DispatcherKeepsItsOwnNarrowerChecks) {
#define NEVERD_KERNEL_DISPATCHER_API(Name, Arity) expectLimit(#Name, 15);
#include "windows/KernelDispatcherAPIs.def"
#undef NEVERD_KERNEL_DISPATCHER_API
}

TEST(DriverKernelAPIIRQL, UnknownAndInexactSpellingsHaveNoGuessedPolicy) {
  const llvm::StringLiteral Names[] = {
      "",           "ZwNotModeled",           "ZwOpenKeyEx",  "zwopenkey",
      "ZwOpenKey ", "ntoskrnl.exe!ZwOpenKey", "IoCallDriverEx", "DbgPrintEx2",
      "memcpy_s"};
  for (auto Name : Names) {
    auto Maximum = maximumKernelIRQL(Name);
    if (Maximum) {
      ADD_FAILURE() << "unexpected policy for " << Name.str();
      continue;
    }
    EXPECT_NE(
        llvm::toString(Maximum.takeError()).find("no kernel IRQL contract"),
        std::string::npos);
  }
}

} // namespace
} // namespace neverd::emulation
