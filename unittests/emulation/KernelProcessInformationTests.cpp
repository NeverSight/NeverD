//===- KernelProcessInformationTests.cpp - Process snapshot contracts -----===//
///
/// \file
/// Process snapshot contracts.
///

#include "ProcessPebTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelReferenceImage.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <cstdlib>
#include <filesystem>
#include <tuple>

namespace neverd::emulation {
namespace {
DriverOptions processOptions() {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.ProcessInventory = DriverProcessInventory{
      {4, 0, "System"}, {100, 4, "Client.exe", 0x401000, 0x180004000}};
  for (auto &P : *O.ProcessInventory) {
    P.ImageFileName.emplace();
    std::copy(P.ImageName.begin(), P.ImageName.end(), P.ImageFileName->begin());
  }
  return O;
}
TEST(DriverProcessInventoryScenario, ValidatesAndRoundTripsExplicitFacts) {
  auto O = driverOptionsFromScenarioJSON(R"({
    "windows_version":{"major":10,"minor":0,"build":19041},
    "process_inventory":[{"process_id":4,"image_name":"System"},
    {"process_id":100,"parent_id":4,"image_name":"Client.exe",
     "borrowed_thread_start":4198400,"borrowed_thread_system_start":6442467328,"base_priority":12,
     "handle_count":17,"virtual_size":18446744073709551615,
     "image_file_name_hex":"436c69656e742e6578650000000000"}]})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->ProcessInventory);
  EXPECT_EQ(O->ProcessInventory->at(1).VirtualSize, UINT64_MAX);
  DriverResult R;
  R.Configuration = *O;
  auto J = llvm::json::parse(driverResultJSON(R));
  ASSERT_TRUE(bool(J));
  auto *C = J->getAsObject()->getObject("configuration");
  ASSERT_NE(C, nullptr);
  auto Copy = driverOptionsFromScenarioJSON(
      R"({"windows_version":{"major":10,"minor":0,"build":19041},"process_inventory":)" +
      llvm::formatv("{0}", *C->get("process_inventory")).str() + "}");
  ASSERT_TRUE(bool(Copy)) << llvm::toString(Copy.takeError());
  ASSERT_TRUE(Copy->ProcessInventory);
  const auto &P = Copy->ProcessInventory->at(1);
  EXPECT_EQ(P.ProcessID, 100u);
  EXPECT_EQ(P.ParentID, 4u);
  EXPECT_EQ(P.ImageName, "Client.exe");
  EXPECT_EQ(P.BorrowedThreadStart, 0x401000u);
  EXPECT_EQ(P.BorrowedThreadSystemStart, 0x180004000u);
  EXPECT_EQ(P.BasePriority, 12);
  EXPECT_EQ(P.HandleCount, 17u);
  EXPECT_EQ(P.VirtualSize, UINT64_MAX);
  ASSERT_TRUE(P.ImageFileName);
  EXPECT_EQ(P.ImageFileName->at(0), 'C');
  EXPECT_EQ(P.ImageFileName->at(10), 0u);
  auto Inherited = driverOptionsFromScenarioJSON("{}", *O);
  ASSERT_TRUE(bool(Inherited));
  EXPECT_EQ(Inherited->ProcessInventory->size(), 2u);
  auto Absent = driverOptionsFromScenarioJSON("{}");
  ASSERT_TRUE(bool(Absent));
  EXPECT_FALSE(Absent->ProcessInventory);
}
TEST(DriverProcessInventoryScenario, InvalidAndIncompleteMetadataFailClearly) {
  const std::string Prefix =
      R"({"windows_version":{"major":10,"minor":0,"build":19041},"process_inventory":)";
  for (
      const char *Bad :
      {"null", "true", "{}", "[]", R"([{"process_id":4,"image_name":"Wrong"}])",
       R"([{"process_id":4,"image_name":"System","unknown":0}])",
       R"([{"process_id":4,"image_name":"System","base_priority":32}])",
       R"([{"process_id":4,"image_name":"System","handle_count":-1}])",
       R"([{"process_id":4,"image_name":"System","session_id":4294967296}])",
       R"([{"process_id":4,"image_name":"System","image_file_name_hex":"00"}])",
       R"([{"process_id":4,"image_name":"System","image_file_name_hex":null}])",
       R"([{"process_id":4,"image_name":"System","image_file_name_hex":"zz0000000000000000000000000000"}])",
       R"([{"process_id":4,"image_name":"System","borrowed_thread_start":140737488355328}])",
       R"([{"process_id":100,"image_name":"Client.exe"}])",
       R"([{"process_id":4,"image_name":"System"},{"process_id":4,"image_name":"System"}])",
       R"([{"process_id":4,"image_name":"System"},{"process_id":100,"parent_id":100,"image_name":"A"}])"}) {
    auto O = driverOptionsFromScenarioJSON(Prefix + Bad + "}");
    ASSERT_FALSE(bool(O)) << Bad;
    EXPECT_NE(llvm::toString(O.takeError()).find("process_inventory"),
              std::string::npos);
  }
  auto O = driverOptionsFromScenarioJSON(
      R"({"process_inventory":[{"process_id":4,"image_name":"System"}]})");
  ASSERT_FALSE(bool(O));
  EXPECT_NE(llvm::toString(O.takeError()).find("19041"), std::string::npos);
  auto Typed = processOptions();
  Typed.ProcessInventory->at(1).ImageName = std::string(513, 'a');
  auto TooLong = validateDriverProcessInventory(Typed);
  ASSERT_TRUE(bool(TooLong));
  llvm::consumeError(std::move(TooLong));
  Typed.ProcessInventory->at(1).ImageName = "Client.exe";
  DriverRequest Request;
  Request.RequestorProcessID = 200;
  Typed.Requests.push_back(Request);
  auto E = validateDriverProcessInventory(Typed);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("missing metadata"),
            std::string::npos);
}
class KernelProcessInformation : public testing::Test {
protected:
  static constexpr uint64_t B = 0x60000000, Returned = B + 0x10000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void initialize(DriverOptions O = processOptions(),
                  const KernelReferenceImage *Reference = nullptr) {
    Model.reset();
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend));
    Memory = std::move(*Backend);
    ok(Memory->map(B, 0x11000, Read | Write));
    ok(Memory->map(0x180001000, 4096, Read | Execute));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    if (Reference)
      ok(Model->setDebuggerReference(*Reference));
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = 0x180001000;
    I.Size = 0x4000;
    ok(Model->initialize(I, O));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  uint64_t call(llvm::StringRef Name, std::initializer_list<uint64_t> A = {}) {
    auto R = Model->call(Name.str(), A);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  uint64_t get(uint64_t At, unsigned Size = 8) {
    auto R = Memory->readInteger(At, Size);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  uint64_t snapshot() {
    EXPECT_EQ(call("ZwQuerySystemInformation", {5, 0, 0, Returned}),
              0xc0000004u);
    const auto Size = get(Returned, 4);
    EXPECT_EQ(call("ZwQuerySystemInformation", {5, B, Size, Returned}), 0u);
    return Size;
  }
};
TEST_F(KernelProcessInformation,
       LinkedRowsAndThreadIDsUseSharedKernelIdentity) {
  auto O = processOptions();
  O.ProcessInventory->at(1).ImageName = "\xe6\xb5\x8b\xe8\xaf\x95.exe";
  O.ProcessInventory->at(1).HandleCount = 17;
  O.ProcessInventory->at(1).WorkingSetSize = 0x12345000;
  initialize(O);
  const auto Current = call("PsGetCurrentThread");
  const auto TID = call("PsGetCurrentThreadId");
  EXPECT_EQ(call("KeSetPriorityThread", {Current, 12}), 8u);
  const auto Size = snapshot();
  EXPECT_EQ(get(B + 4, 4), 1u);
  EXPECT_EQ(get(B + 0x50), 4u);
  EXPECT_EQ(get(B + 0x100 + 0x20), 0x180001000u);
  EXPECT_EQ(get(B + 0x100 + 0x28), 4u);
  EXPECT_EQ(get(B + 0x100 + 0x30), TID);
  EXPECT_EQ(get(B + 0x100 + 0x38, 4), 12u);
  EXPECT_EQ(get(B + 0x100 + 0x3c, 4), 8u);
  EXPECT_EQ(get(B + 0x100 + 0x44, 4), 2u);
  const auto Next = get(B, 4);
  ASSERT_GT(Next, 0u);
  ASSERT_LT(Next, Size);
  EXPECT_EQ(Next % 8, 0u);
  EXPECT_EQ(get(B + Next, 4), 0u);
  EXPECT_EQ(get(B + Next + 4, 4), 0u);
  EXPECT_EQ(get(B + Next + 0x50), 100u);
  EXPECT_EQ(get(B + Next + 0x58), 4u);
  EXPECT_EQ(get(B + Next + 0x60, 4), 17u);
  EXPECT_EQ(get(B + Next + 0x90), 0x12345000u);
  EXPECT_EQ(get(B + Next + 0x38, 2), 12u);
  const auto Name = get(B + Next + 0x40);
  EXPECT_EQ(Name, B + Next + 0x100);
  EXPECT_EQ(get(Name, 2), 0x6d4bu);
  EXPECT_EQ(get(Name + 2, 2), 0x8bd5u);
  EXPECT_EQ(get(Name + 12, 2), 0u);
  EXPECT_EQ(call("PsLookupThreadByThreadId", {TID, Returned + 8}), 0u);
  EXPECT_EQ(get(Returned + 8), Current);
  call("ObfDereferenceObject", {Current});
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned + 8}), 0u);
  const auto Process = get(Returned + 8);
  EXPECT_EQ(call("PsGetProcessId", {Process}), 100u);
  call("ObfDereferenceObject", {Process});
}
TEST_F(KernelProcessInformation, LengthProbeAliasLowULONGAndOutputPreflight) {
  initialize();
  ok(Memory->writeInteger(Returned, 0xfeedface, 4));
  EXPECT_EQ(call("ZwQuerySystemInformation", {5, Returned, 0, Returned}),
            0xc0000004u);
  const auto Size = get(Returned, 4);
  ASSERT_LT(Size, 4096u);
  ok(Memory->writeInteger(B, 0x123456789abcdef0ULL, 8));
  EXPECT_EQ(call("ZwQuerySystemInformation", {5, B, Size - 1, Returned}),
            0xc0000004u);
  EXPECT_EQ(get(B), 0x123456789abcdef0ULL);
  EXPECT_EQ(call("ZwQuerySystemInformation", {5, B, Size + (1ULL << 32), B}),
            0u);
  EXPECT_EQ(get(B, 4), Size);
  EXPECT_EQ(call("NtQuerySystemInformation", {5, B, Size, 0}), 0u);
  ok(Memory->writeInteger(B, 0x123456789abcdef0ULL, 8));
  ok(Memory->protect(Returned, 4096, Read));
  auto R = Model->call("ZwQuerySystemInformation", {5, B, Size, Returned});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(get(B), 0x123456789abcdef0ULL);
  ok(Memory->protect(Returned, 4096, Read | Write));
  ok(Memory->writeInteger(Returned, 0xfeedface, 4));
  ok(Memory->protect(B, 4096, Read));
  R = Model->call("ZwQuerySystemInformation", {5, B, Size, Returned});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(get(Returned, 4), 0xfeedfaceu);
  EXPECT_EQ(call("ZwQuerySystemInformation", {5, B, 0, Returned}), 0xc0000004u);
}
TEST_F(KernelProcessInformation, LifecyclePublicationAndExitMatchPIDLookup) {
  auto O = processOptions();
  O.ProcessEvents = {{DriverProcessEvent::Kind::Create, 100, 4, 4, 0},
                     {DriverProcessEvent::Kind::Exit, 100, 4, 4, 0}};
  initialize(O);
  snapshot();
  EXPECT_EQ(get(B, 4), 0u);
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned + 8}),
            0xc000000bu);
  auto Event = Model->beginProcessEvent(O.ProcessEvents[0], 0);
  ASSERT_TRUE(bool(Event)) << llvm::toString(Event.takeError());
  EXPECT_FALSE(*Event);
  ok(Model->completeProcessEvent());
  snapshot();
  EXPECT_GT(get(B, 4), 0u);
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned + 8}), 0u);
  const auto Object = get(Returned + 8);
  Event = Model->beginProcessEvent(O.ProcessEvents[1], 1);
  ASSERT_TRUE(bool(Event));
  EXPECT_FALSE(*Event);
  ok(Model->completeProcessEvent());
  snapshot();
  EXPECT_EQ(get(B, 4), 0u);
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned + 16}),
            0xc000000bu);
  EXPECT_EQ(call("PsGetProcessId", {Object}), 100u);
  call("ObfDereferenceObject", {Object});
}
TEST_F(KernelProcessInformation,
       CreatedThreadUsesLivePriorityAndDisappearsOnExit) {
  initialize();
  const auto RootTID = call("PsGetCurrentThreadId");
  EXPECT_EQ(call("PsCreateSystemThread",
                 {Returned, 0x1fffff, 0, 0, Returned + 0x20, 0x180001000, 0}),
            0u);
  const auto Handle = get(Returned);
  const auto TID = get(Returned + 0x28);
  EXPECT_NE(TID, RootTID);
  EXPECT_EQ(call("PsLookupThreadByThreadId", {TID, Returned + 8}), 0u);
  const auto Object = get(Returned + 8);
  EXPECT_EQ(call("KeSetPriorityThread", {Object, 11}), 8u);
  snapshot();
  EXPECT_EQ(get(B + 4, 4), 2u);
  const auto Thread = B + 0x100 + 0x50;
  EXPECT_EQ(get(Thread + 0x30), TID);
  EXPECT_EQ(get(Thread + 0x38, 4), 11u);
  EXPECT_EQ(get(Thread + 0x44, 4), 1u);
  auto Next = Model->nextScheduled(false);
  ASSERT_TRUE(bool(Next)) << llvm::toString(Next.takeError());
  ASSERT_TRUE(*Next);
  Model->enterExecution(B + 0x8000, (**Next).ID);
  snapshot();
  EXPECT_EQ(get(Thread + 0x44, 4), 2u);
  ok(Memory->writeInteger(Returned + 0x40, uint64_t(-100), 8));
  call("KeDelayExecutionThread", {0, 0, Returned + 0x40});
  const auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  Model->enterForeground();
  Model->enterExecution(profile::StackBase);
  snapshot();
  EXPECT_EQ(get(Thread + 0x44, 4), 5u);
  EXPECT_EQ(get(Thread + 0x48, 4), 4u);
  ASSERT_TRUE(Wait->Deadline);
  ok(Model->advanceExecutionTo100ns(*Wait->Deadline));
  auto Ready = Model->pollWait(*Wait);
  ASSERT_TRUE(bool(Ready));
  ASSERT_TRUE(*Ready);
  EXPECT_EQ(**Ready, 0u);
  Model->enterExecution(B + 0x8000, (**Next).ID);
  call("PsTerminateSystemThread", {0});
  ASSERT_EQ(Model->takeThreadTermination(), 0u);
  ok(Model->finishScheduled((**Next).ID));
  Model->enterForeground();
  Model->enterExecution(profile::StackBase);
  snapshot();
  EXPECT_EQ(get(B + 4, 4), 1u);
  EXPECT_EQ(call("PsLookupThreadByThreadId", {TID, Returned + 16}),
            0xc000000bu);
  // A reference can outlive enumeration membership.
  EXPECT_EQ(call("PsGetThreadId", {Object}), TID);
  call("ZwClose", {Handle});
  call("ObfDereferenceObject", {Object});
}
TEST_F(KernelProcessInformation, MissingFactsAndUserModeNeverBorrowHostState) {
  auto O = processOptions();
  O.ProcessInventory.reset();
  initialize(O);
  auto R = Model->call("ZwQuerySystemInformation", {5, 0, 0, Returned});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("process_inventory"),
            std::string::npos);
  initialize();
  ok(Model->setUserRequestContext(true, 100));
  R = Model->call("NtQuerySystemInformation", {5, B, 4096, Returned});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("UserMode"), std::string::npos);
  snapshot();
  const auto Next = get(B, 4);
  EXPECT_EQ(get(B + Next + 4, 4), 1u);
  EXPECT_EQ(get(B + Next + 0x100 + 0x20), 0x180004000u);
  EXPECT_EQ(get(B + Next + 0x100 + 0x28), 100u);
  // API class 9 retains the distinct Win32 entry through its existing owner.
  const auto Current = call("PsGetCurrentThread");
  EXPECT_EQ(call("PsGetThreadId", {Current}), get(B + Next + 0x100 + 0x30));
  ok(Model->setUserRequestContext(true, 200));
  R = Model->call("ZwQuerySystemInformation", {5, 0, 0, Returned});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("missing metadata"),
            std::string::npos);
}
TEST_F(KernelProcessInformation,
       UnknownBorrowedStartAndWrongVersionAreExplicit) {
  auto O = processOptions();
  O.ProcessInventory->at(1).BorrowedThreadSystemStart.reset();
  initialize(O);
  ok(Model->setUserRequestContext(true, 100));
  auto R = Model->call("ZwQuerySystemInformation", {5, 0, 0, Returned});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("start address"),
            std::string::npos);
  O = processOptions();
  O.ProcessInventory.reset();
  O.Version.reset();
  initialize(O);
  R = Model->call("ZwQuerySystemInformation", {5, 0, 0, Returned});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("19041"), std::string::npos);
}
TEST_F(KernelProcessInformation, ShortNameReturnsStableReadOnlyNativeField) {
  initialize();
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned}), 0u);
  const auto Object = get(Returned);
  const auto At = call("PsGetProcessImageFileName", {Object});
  EXPECT_EQ(At, Object + 0x5a8);
  EXPECT_EQ(call("PsGetProcessImageFileName", {Object}), At);
  EXPECT_EQ(get(At, 8), 0x652e746e65696c43ULL);
  EXPECT_EQ(get(At + 8, 2), 0x6578u);
  EXPECT_EQ(get(At + 10, 1), 0u);
  ok(Model->validateGuestAccess(At, 15, false));
  for (auto [Address, Size, Write] :
       {std::tuple<uint64_t, uint32_t, bool>{At, 15, true},
        {At - 1, 1, false},
        {At + 15, 1, false},
        {At, 16, false}}) {
    auto E = Model->validateGuestAccess(Address, Size, Write);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  call("ObfDereferenceObject", {Object});
  auto Bad = Model->call("PsGetProcessImageFileName", {Returned});
  ASSERT_FALSE(bool(Bad));
  EXPECT_NE(llvm::toString(Bad.takeError()).find("known process object"),
            std::string::npos);
}
TEST_F(KernelProcessInformation,
       ShortNameBytesDoNotInferEncodingOrTermination) {
  auto O = processOptions();
  O.ProcessInventory->at(1).ImageFileName->fill(0xff);
  initialize(O);
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned}), 0u);
  const auto Object = get(Returned);
  const auto At = call("PsGetProcessImageFileName", {Object});
  EXPECT_EQ(get(At), UINT64_MAX);
  EXPECT_EQ(get(At + 14, 1), 0xffu);
  call("ObfDereferenceObject", {Object});
  O.ProcessInventory->at(0).ImageFileName.reset();
  initialize(O);
  const auto System = call("IoGetCurrentProcess");
  auto Missing = Model->call("PsGetProcessImageFileName", {System});
  ASSERT_FALSE(bool(Missing));
  EXPECT_NE(llvm::toString(Missing.takeError()).find("image_file_name_hex"),
            std::string::npos);
}
TEST(DriverProcessSectionBaseScenario, DistinguishesUnknownZeroAnd64BitValues) {
  const std::string Prefix =
      R"({"windows_version":{"major":10,"minor":0,"build":19041},"process_inventory":[{"process_id":4,"image_name":"System","section_base_address":)";
  for (const char *Bad :
       {"null", "true", "-1", "140737488355328", "18446744073709551616"}) {
    auto O = driverOptionsFromScenarioJSON(Prefix + Bad + "}]}");
    ASSERT_FALSE(bool(O)) << Bad;
    llvm::consumeError(O.takeError());
  }
  for (uint64_t Value : {uint64_t(0), uint64_t(0x140000000), UINT64_MAX}) {
    auto O =
        driverOptionsFromScenarioJSON(Prefix + std::to_string(Value) + "}]}");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    ASSERT_TRUE(O->ProcessInventory->at(0).SectionBaseAddress);
    EXPECT_EQ(*O->ProcessInventory->at(0).SectionBaseAddress, Value);
    DriverResult R;
    R.Configuration = *O;
    auto JSON = llvm::json::parse(driverResultJSON(R));
    ASSERT_TRUE(bool(JSON));
    const auto &V = *JSON->getAsObject()
                         ->getObject("configuration")
                         ->get("process_inventory");
    auto Copy = driverOptionsFromScenarioJSON(
        R"({"windows_version":{"major":10,"minor":0,"build":19041},"process_inventory":)" +
        llvm::formatv("{0}", V).str() + "}");
    ASSERT_TRUE(bool(Copy));
    EXPECT_EQ(Copy->ProcessInventory->at(0).SectionBaseAddress, Value);
  }
  EXPECT_FALSE(processOptions().ProcessInventory->at(0).SectionBaseAddress);
  auto Typed = processOptions();
  Typed.ProcessInventory->at(0).SectionBaseAddress = 0x800000000000ULL;
  auto E = validateDriverProcessInventory(Typed);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST_F(KernelProcessInformation, SectionBaseSharesExactReadOnlyFieldBytes) {
  auto O = processOptions();
  O.ProcessInventory->at(0).SectionBaseAddress = 0;
  O.ProcessInventory->at(1).SectionBaseAddress = 0x140000000;
  initialize(O);
  EXPECT_EQ(Model->argumentCount("PsGetProcessSectionBaseAddress"), 1u);
  const auto System = call("IoGetCurrentProcess");
  EXPECT_EQ(call("PsGetProcessSectionBaseAddress", {System}), 0u);
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned}), 0u);
  const auto User = get(Returned),
             Field = User + windows::ProcessSectionBase19041Offset;
  EXPECT_EQ(call("PsGetProcessSectionBaseAddress", {User}), 0x140000000u);
  EXPECT_EQ(get(Field), 0x140000000u);
  for (unsigned I = 0; I < 8; ++I)
    ok(Model->validateGuestAccess(Field + I, 8 - I, false));
  for (auto [At, Size, Write] :
       {std::tuple<uint64_t, uint32_t, bool>{Field, 8, true},
        {Field - 1, 2, false},
        {Field + 7, 2, false},
        {Field + 8, 1, false}}) {
    auto E = Model->validateGuestAccess(At, Size, Write);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  call("KfRaiseIrql", {15});
  EXPECT_EQ(call("PsGetProcessSectionBaseAddress", {User}), 0x140000000u);
  call("KeLowerIrql", {0});
  call("ObfDereferenceObject", {User});
}
TEST_F(KernelProcessInformation,
       SectionBaseRejectsMissingFactsAndUnknownObjects) {
  initialize(processOptions());
  const auto System = call("IoGetCurrentProcess");
  auto Missing = Model->call("PsGetProcessSectionBaseAddress", {System});
  ASSERT_FALSE(bool(Missing));
  EXPECT_NE(llvm::toString(Missing.takeError()).find("section_base_address"),
            std::string::npos);
  for (uint64_t Pointer : {uint64_t(0), Returned, call("PsGetCurrentThread")}) {
    auto R = Model->call("PsGetProcessSectionBaseAddress", {Pointer});
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("known process object"),
              std::string::npos);
  }
  auto Bad = Model->call("PsGetProcessSectionBaseAddress", {});
  ASSERT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
  DriverOptions O;
  initialize(O);
  Missing = Model->call("PsGetProcessSectionBaseAddress",
                        {call("IoGetCurrentProcess")});
  ASSERT_FALSE(bool(Missing));
  EXPECT_NE(llvm::toString(Missing.takeError()).find("19041"),
            std::string::npos);
}
TEST_F(KernelProcessInformation,
       SectionBaseRejectsUnauthenticatedReferenceGetter) {
  auto O = processOptions();
  O.ProcessInventory->at(0).SectionBaseAddress = 0;
  auto Reference =
      KernelReferenceImage::load(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                     "driver_process_data_reference.sys",
                                 O.MemoryLimit);
  ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
  initialize(O, &*Reference);
  const auto Process = call("IoGetCurrentProcess");
  auto R = Model->call("PsGetProcessSectionBaseAddress", {Process});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("reference getter"),
            std::string::npos);
  auto E = Model->validateGuestAccess(
      Process + windows::ProcessSectionBase19041Offset, 8, false);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST(DriverProcessSectionBase, StaticDynamicAttachedAndLifecycleClients) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (unsigned Reference : {0u, 1u, 2u}) {
        const char *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE");
        if (Reference && !Image)
          continue;
        const char *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
        if (Reference == 2 && !PDB)
          continue;
        auto O = processOptions();
        O.Contract = Contract;
        O.Unload = true;
        O.ProcessInventory->at(0).SectionBaseAddress = 0;
        O.ProcessInventory->at(1).SectionBaseAddress = 0x140000000;
        O.ProcessInventory->push_back({200, 100, "Child.exe"});
        O.ProcessInventory->back().SectionBaseAddress = 0x400000;
        O.ProcessEvents = {{DriverProcessEvent::Kind::Create, 200, 100, 4, 0},
                           {DriverProcessEvent::Kind::Exit, 200, 100, 4, 0}};
        DriverRequest Request;
        Request.Kind = DriverRequestKind::Create;
        Request.RequestorProcessID = 100;
        O.Requests.push_back(Request);
        Request.Kind = DriverRequestKind::Cleanup;
        O.Requests.push_back(Request);
        Request.Kind = DriverRequestKind::Close;
        O.Requests.push_back(Request);
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        if (Reference) {
          O.KernelImage = Image;
          if (Reference == 2)
            O.KernelPDB = PDB;
        }
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_process_section_base.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        ASSERT_EQ(R->ProcessEvents.size(), 2u);
        for (const auto &E : R->ProcessEvents)
          EXPECT_TRUE(E.Completed);
      }
}
TEST(DriverProcessExitStatusScenario, ExitCodesAreExitOnlyAndRoundTrip) {
  for (const char *Bad : {"null", "true", "-1", "4294967296"}) {
    auto O = driverOptionsFromScenarioJSON(
        std::string(
            R"({"process_events":[{"kind":"create","process_id":100,"parent_id":4},{"kind":"exit","process_id":100,"parent_id":4,"exit_status":)") +
        Bad + "}]}");
    ASSERT_FALSE(bool(O));
    llvm::consumeError(O.takeError());
  }
  auto Bad = driverOptionsFromScenarioJSON(
      R"({"process_events":[{"kind":"create","process_id":100,"parent_id":4,"exit_status":0}]})");
  ASSERT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
  for (uint32_t Code : {0u, 0xc0000005u, UINT32_MAX, 0x103u}) {
    auto O = driverOptionsFromScenarioJSON(
        std::string(
            R"({"process_events":[{"kind":"create","process_id":100,"parent_id":4},{"kind":"exit","process_id":100,"parent_id":4,"exit_status":)") +
        std::to_string(Code) + "}]}");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    EXPECT_EQ(O->ProcessEvents[1].ExitStatus, Code);
    DriverResult R;
    R.Configuration = *O;
    auto JSON = llvm::json::parse(driverResultJSON(R));
    ASSERT_TRUE(bool(JSON));
    auto *Events = JSON->getAsObject()
                       ->getObject("configuration")
                       ->getArray("process_events");
    ASSERT_NE(Events, nullptr);
    EXPECT_EQ((*Events)[1].getAsObject()->getInteger("exit_status"),
              int64_t(Code));
  }
  DriverOptions O;
  O.ProcessEvents = {{DriverProcessEvent::Kind::Create, 100, 4, 4, 0, 0}};
  auto E = validateDriverProcessEvents(O.ProcessEvents, O.Requests);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST_F(KernelProcessInformation, ProcessExitStatusPendingFieldAndIRQL) {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  initialize(O);
  const auto Process = call("IoGetCurrentProcess");
  const auto Field = Process + windows::ProcessExitStatus19041Offset;
  EXPECT_EQ(Model->argumentCount("PsGetProcessExitStatus"), 1u);
  EXPECT_EQ(call("PsGetProcessExitStatus", {Process}), 0x103u);
  EXPECT_EQ(get(Field, 4), 0x103u);
  for (unsigned I = 0; I < 4; ++I)
    ok(Model->validateGuestAccess(Field + I, 4 - I, false));
  for (auto [At, Size, Write] :
       {std::tuple<uint64_t, uint32_t, bool>{Field, 4, true},
        {Field - 1, 2, false},
        {Field + 3, 2, false},
        {Field + 4, 1, false}}) {
    auto E = Model->validateGuestAccess(At, Size, Write);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  call("KfRaiseIrql", {1});
  EXPECT_EQ(call("PsGetProcessExitStatus", {Process}), 0x103u);
  call("KeLowerIrql", {0});
  call("KfRaiseIrql", {2});
  auto V = Model->call("PsGetProcessExitStatus", {Process});
  ASSERT_FALSE(bool(V));
  EXPECT_NE(llvm::toString(V.takeError()).find("IRQL"), std::string::npos);
  call("KeLowerIrql", {0});
}
TEST_F(KernelProcessInformation,
       ProcessExitStatusChangesBeforeRetirementAndSurvivesIt) {
  for (uint32_t Code : {0u, 0xc0000005u, UINT32_MAX, 0x103u}) {
    initialize();
    EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned}), 0u);
    const auto Process = get(Returned),
               Field = Process + windows::ProcessExitStatus19041Offset;
    EXPECT_EQ(call("PsGetProcessExitStatus", {Process}), 0x103u);
    auto Event = Model->beginProcessEvent(
        {DriverProcessEvent::Kind::Exit, 100, 4, 4, 0, Code}, 0);
    ASSERT_TRUE(bool(Event)) << llvm::toString(Event.takeError());
    EXPECT_FALSE(*Event);
    EXPECT_EQ(call("PsGetProcessExitStatus", {Process}), Code);
    EXPECT_EQ(get(Field, 4), Code);
    ok(Model->completeProcessEvent());
    EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned}), 0xc000000bu);
    EXPECT_EQ(call("PsGetProcessExitStatus", {Process}), Code);
    EXPECT_EQ(get(Field, 4), Code);
    call("ObfDereferenceObject", {Process});
  }
}
TEST_F(KernelProcessInformation,
       ProcessExitStatusUnknownRevokesPublishedPendingBytes) {
  initialize();
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned}), 0u);
  const auto Process = get(Returned),
             Field = Process + windows::ProcessExitStatus19041Offset;
  EXPECT_EQ(call("PsGetProcessExitStatus", {Process}), 0x103u);
  auto Event = Model->beginProcessEvent(
      {DriverProcessEvent::Kind::Exit, 100, 4, 4, 0}, 0);
  ASSERT_TRUE(bool(Event));
  auto V = Model->call("PsGetProcessExitStatus", {Process});
  ASSERT_FALSE(bool(V));
  EXPECT_NE(llvm::toString(V.takeError()).find("unknown"), std::string::npos);
  auto E = Model->validateGuestAccess(Field, 4, false);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  ok(Model->completeProcessEvent());
  V = Model->call("PsGetProcessExitStatus", {Process});
  ASSERT_FALSE(bool(V));
  llvm::consumeError(V.takeError());
  call("ObfDereferenceObject", {Process});
}
TEST_F(KernelProcessInformation,
       ProcessExitStatusRejectsUnknownObjectsAndReferenceLayout) {
  initialize();
  for (uint64_t At : {uint64_t(0), Returned, call("PsGetCurrentThread")}) {
    auto R = Model->call("PsGetProcessExitStatus", {At});
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("known process object"),
              std::string::npos);
  }
  auto R = Model->call("PsGetProcessExitStatus", {});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  initialize(DriverOptions{});
  R = Model->call("PsGetProcessExitStatus", {call("IoGetCurrentProcess")});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("19041"), std::string::npos);
  auto Reference =
      KernelReferenceImage::load(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                     "driver_process_data_reference.sys",
                                 64 * 1024 * 1024);
  ASSERT_TRUE(bool(Reference));
  initialize(processOptions(), &*Reference);
  R = Model->call("PsGetProcessExitStatus", {call("IoGetCurrentProcess")});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("reference getter"),
            std::string::npos);
}
TEST(DriverProcessExitStatus, StaticDynamicAttachedAndRetainedExitClients) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (unsigned Reference : {0u, 1u, 2u}) {
        const char *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE");
        const char *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
        if ((Reference && !Image) || (Reference == 2 && !PDB))
          continue;
        DriverOptions O;
        O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
        O.Contract = Contract;
        O.Unload = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        if (Reference)
          O.KernelImage = Image;
        if (Reference == 2)
          O.KernelPDB = PDB;
        DriverRequest Request;
        Request.RequestorProcessID = 100;
        for (auto Kind : {DriverRequestKind::Create, DriverRequestKind::Cleanup,
                          DriverRequestKind::Close}) {
          Request.Kind = Kind;
          O.Requests.push_back(Request);
        }
        O.ProcessEvents = {
            {DriverProcessEvent::Kind::Create, 100, 4, 4, 0},
            {DriverProcessEvent::Kind::Exit, 100, 4, 4, 3, 0xc0000005}};
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_process_exit_status.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        ASSERT_EQ(R->Requests.size(), 3u);
        for (const auto &Q : R->Requests)
          EXPECT_TRUE(Q.Completed);
        ASSERT_EQ(R->ProcessEvents.size(), 2u);
        for (const auto &E : R->ProcessEvents) {
          EXPECT_TRUE(E.Completed);
          EXPECT_EQ(E.DeliveredRoutines.size(), 1u);
        }
      }
}
TEST(DriverProcessInformation, CompiledClientWalksLiveProcessesAndThreads) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    auto O = processOptions();
    O.Contract = Contract;
    O.Unload = true;
    auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_process_information.sys",
                           O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
    EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
    EXPECT_TRUE(R->UnloadCompleted);
  }
}

TEST(DriverProcessPebScenario, NullUnknownAndUserPointersRoundTrip) {
  auto O = test::processPebOptions();
  EXPECT_FALSE(O.ProcessInventory->at(0).PebAddress);
  for (uint64_t Value : {uint64_t(0), uint64_t(0x7ffffffe0123)}) {
    O.ProcessInventory->at(1).PebAddress = Value;
    DriverResult Result;
    Result.Configuration = O;
    auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
    const auto &Inventory = *JSON.getAsObject()
                                 ->getObject("configuration")
                                 ->get("process_inventory");
    auto Copy = driverOptionsFromScenarioJSON(
        R"({"windows_version":{"major":10,"minor":0,"build":19041},"process_inventory":)" +
        llvm::formatv("{0}", Inventory).str() + "}");
    ASSERT_TRUE(bool(Copy)) << llvm::toString(Copy.takeError());
    EXPECT_EQ(Copy->ProcessInventory->at(1).PebAddress, Value);
    EXPECT_FALSE(Copy->ProcessInventory->at(0).PebAddress);
  }
  for (const char *Bad : {"null", "true", "-1", "140737488355328",
                          "18446744073709551615", "18446744073709551616"}) {
    auto Parsed = driverOptionsFromScenarioJSON(
        std::string(
            R"({"windows_version":{"major":10,"minor":0,"build":19041},"process_inventory":[{"process_id":4,"image_name":"System"},{"process_id":100,"image_name":"A","peb_address":)") +
        Bad + "}]}");
    ASSERT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
  O.ProcessInventory->at(0).PebAddress = 0x400000;
  auto E = validateDriverProcessInventory(O);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("System has no PEB"),
            std::string::npos);
}
TEST_F(KernelProcessInformation, PebPointerSharesExactReadOnlyObjectBytes) {
  auto O = test::processPebOptions();
  initialize(O);
  const auto System = call("IoGetCurrentProcess");
  EXPECT_EQ(call("PsGetProcessPeb", {System}), 0u);
  EXPECT_EQ(get(System + windows::ProcessPeb19041Offset), 0u);
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned}), 0u);
  const auto User = get(Returned),
             Field = User + windows::ProcessPeb19041Offset;
  EXPECT_EQ(call("PsGetProcessPeb", {User}), 0x400000u);
  EXPECT_EQ(get(Field), 0x400000u);
  for (unsigned I = 0; I < 8; ++I)
    ok(Model->validateGuestAccess(Field + I, 8 - I, false));
  for (auto [At, Size, Write] :
       {std::tuple<uint64_t, uint32_t, bool>{Field, 8, true},
        {Field - 1, 2, false},
        {Field + 7, 2, false},
        {Field + 8, 1, false}}) {
    auto E = Model->validateGuestAccess(At, Size, Write);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  call("KfRaiseIrql", {15});
  EXPECT_EQ(call("PsGetProcessPeb", {User}), 0x400000u);
  call("KeLowerIrql", {0});
  // The pointer does not activate another process's user address space.
  EXPECT_FALSE(llvm::cantFail(Memory->canAccess(0x400000, 8, Read)));
  const auto Apc = call("ExAllocatePoolWithTag", {0, 0x30, 0x50454254});
  call("KeStackAttachProcess", {User, Apc});
  EXPECT_EQ(call("PsGetProcessPeb", {call("IoGetCurrentProcess")}), 0x400000u);
  EXPECT_EQ(get(0x400010), 0x140000000u);
  call("KeUnstackDetachProcess", {Apc});
  EXPECT_FALSE(llvm::cantFail(Memory->canAccess(0x400000, 8, Read)));
  call("ExFreePool", {Apc});
  call("ObfDereferenceObject", {User});
}
TEST_F(KernelProcessInformation, PebRejectsUnknownFactsWithoutInventingMemory) {
  auto O = test::processPebOptions();
  O.ProcessInventory->at(1).PebAddress.reset();
  initialize(O);
  EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned}), 0u);
  const auto User = get(Returned);
  auto R = Model->call("PsGetProcessPeb", {User});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("peb_address"),
            std::string::npos);
  auto E = Model->validateGuestAccess(User + windows::ProcessPeb19041Offset, 8,
                                      false);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  call("ObfDereferenceObject", {User});
  for (uint64_t Pointer : {uint64_t(0), Returned, call("PsGetCurrentThread")}) {
    auto Bad = Model->call("PsGetProcessPeb", {Pointer});
    ASSERT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
  auto Bad = Model->call("PsGetProcessPeb", {});
  ASSERT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
  for (uint64_t Pointer : {uint64_t(0), uint64_t(0x700000)}) {
    O.ProcessInventory->at(1).PebAddress = Pointer;
    O.ProcessInventory->at(1).Memory.reset();
    initialize(O);
    EXPECT_EQ(call("PsLookupProcessByProcessId", {100, Returned}), 0u);
    EXPECT_EQ(call("PsGetProcessPeb", {get(Returned)}), Pointer);
    if (Pointer)
      EXPECT_FALSE(llvm::cantFail(Memory->canAccess(Pointer, 8, Read)));
    call("ObfDereferenceObject", {get(Returned)});
  }
  initialize({});
  auto Missing = Model->call("PsGetProcessPeb", {call("IoGetCurrentProcess")});
  ASSERT_FALSE(bool(Missing));
  EXPECT_NE(llvm::toString(Missing.takeError()).find("19041"),
            std::string::npos);
}
TEST_F(KernelProcessInformation, PebRejectsUnauthenticatedReferenceGetter) {
  auto O = test::processPebOptions();
  auto Reference = llvm::cantFail(
      KernelReferenceImage::load(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                     "driver_process_data_reference.sys",
                                 O.MemoryLimit));
  initialize(O, &Reference);
  auto Process = call("IoGetCurrentProcess");
  auto R = Model->call("PsGetProcessPeb", {Process});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("reference getter"),
            std::string::npos);
  auto E = Model->validateGuestAccess(Process + windows::ProcessPeb19041Offset,
                                      8, false);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST(DriverProcessPeb, CompiledLookupAttachmentIsolationAndRequestClients) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (unsigned Reference : {0u, 1u, 2u}) {
        SCOPED_TRACE(std::to_string(unsigned(Contract)) + ":" +
                     std::to_string(Preemptive) + ":" +
                     std::to_string(Reference));
        const char *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE");
        const char *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
        if ((Reference && !Image) || (Reference == 2 && !PDB))
          continue;
        auto O = test::processPebOptions();
        O.Contract = Contract;
        O.Unload = true;
        O.PageTables = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        if (Reference)
          O.KernelImage = Image;
        if (Reference == 2)
          O.KernelPDB = PDB;
        for (auto Kind : {DriverRequestKind::Create, DriverRequestKind::Cleanup,
                          DriverRequestKind::Close}) {
          DriverRequest R;
          R.Kind = Kind;
          R.RequestorProcessID = 100;
          O.Requests.push_back(R);
        }
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_process_peb.sys",
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
        EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
        EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
        ASSERT_EQ(R->Requests.size(), 3u);
        for (const auto &Request : R->Requests) {
          EXPECT_TRUE(Request.Completed);
          EXPECT_EQ(Request.IOStatus, 0u);
        }
      }
}
} // namespace
} // namespace neverd::emulation
