//===- KernelDriverObjectTests.cpp - Named driver reference ownership
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Named driver reference ownership.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelObjectTypes.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/JSON.h"

#include <tuple>

namespace neverd::emulation {
namespace {
TEST(DriverObjectScenario, NamespaceIsStrictAndPreservesUnknownVsEmpty) {
  for (const char *Value :
       {"null", "[]", "{}", R"({"empty_drivers":null})",
        R"({"empty_drivers":[1]})", R"({"empty_drivers":["\\Device\\X"]})",
        R"({"empty_drivers":["\\Driver\\a","\\driver\\A"]})",
        R"({"empty_drivers":["\\Driver\\NeverDDriver"]})",
        R"({"empty_drivers":[],"unknown":true})"}) {
    auto R = driverOptionsFromScenarioJSON(
        std::string("{\"driver_namespace\":") + Value + "}");
    ASSERT_FALSE(bool(R)) << Value;
    llvm::consumeError(R.takeError());
  }
  auto Unknown = driverOptionsFromScenarioJSON("{}");
  ASSERT_TRUE(bool(Unknown));
  EXPECT_FALSE(Unknown->DriverNamespace);
  auto Empty = driverOptionsFromScenarioJSON(
      R"({"driver_namespace":{"empty_drivers":[]}})");
  ASSERT_TRUE(bool(Empty));
  ASSERT_TRUE(Empty->DriverNamespace);
  EXPECT_TRUE(Empty->DriverNamespace->EmptyDrivers.empty());
  auto Collision = driverOptionsFromScenarioJSON(
      R"({"service_name":"pci","driver_namespace":{"empty_drivers":["\\Driver\\PCI"]}})");
  ASSERT_FALSE(bool(Collision));
  llvm::consumeError(Collision.takeError());
}

TEST(DriverObjectScenario, InventoryRoundTripsAndTypedOversizeFails) {
  auto O = driverOptionsFromScenarioJSON(
      R"({"driver_namespace":{"empty_drivers":["\\Driver\\pci"]}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  auto Base = driverOptionsFromScenarioJSON("{}", *O);
  ASSERT_TRUE(bool(Base));
  EXPECT_EQ(Base->DriverNamespace->EmptyDrivers,
            O->DriverNamespace->EmptyDrivers);
  DriverResult R;
  R.Configuration = *Base;
  auto JSON = llvm::json::parse(driverResultJSON(R));
  ASSERT_TRUE(bool(JSON));
  auto *A = JSON->getAsObject()
                ->getObject("configuration")
                ->getObject("driver_namespace")
                ->getArray("empty_drivers");
  ASSERT_NE(A, nullptr);
  ASSERT_EQ(A->size(), 1u);
  EXPECT_EQ((*A)[0].getAsString(), "\\Driver\\pci");
  DriverOptions Bad;
  Bad.DriverNamespace.emplace();
  Bad.DriverNamespace->EmptyDrivers.resize(65, "\\Driver\\pci");
  auto Parsed = driverOptionsFromScenarioJSON("{}", Bad);
  ASSERT_FALSE(bool(Parsed));
  llvm::consumeError(Parsed.takeError());
}

class KernelDriverObject : public testing::Test {
protected:
  static constexpr uint64_t S = 0x24000000;
  static constexpr uint64_t DT =
      object_type::identity(object_type::Kind::Driver);
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  llvm::Error initialize(DriverOptions O) {
    Model.reset();
    auto B = UnicornBackend::create(8 * 1024 * 1024);
    if (!B)
      return B.takeError();
    Memory = std::move(*B);
    if (auto E = Memory->map(S, 0x10000, Read | Write))
      return E;
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    if (auto E = Model->initialize(I, O))
      return E;
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
    return llvm::Error::success();
  }
  DriverOptions options(bool Closed = true) {
    DriverOptions O;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    if (Closed)
      O.DriverNamespace = DriverObjectNamespace{{"\\Driver\\pci"}};
    return O;
  }
  void SetUp() override { ok(initialize(options())); }
  void put(uint64_t A, uint64_t V, unsigned N = 8) {
    ok(Memory->writeInteger(A, V, N));
  }
  uint64_t get(uint64_t A, unsigned N = 8) {
    auto R = Memory->readInteger(A, N);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> A) {
    auto R = Model->call(Name, A);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  void name(const char *Text) {
    const auto N = std::char_traits<char>::length(Text);
    for (size_t I = 0; I <= N; ++I)
      put(S + 0x100 + I * 2, I == N ? 0 : uint8_t(Text[I]), 2);
    put(S, N * 2, 2);
    put(S + 2, (N + 1) * 2, 2);
    put(S + 8, S + 0x100);
  }
  llvm::Expected<uint64_t> query(uint32_t Flags = 0x240, uint64_t Type = DT,
                                 uint64_t Out = S + 0x300) {
    return Model->call("ObReferenceObjectByName",
                       {S, Flags, 0, 0, Type, 0, 0, Out});
  }
};

TEST_F(KernelDriverObject, OwnAndDeclaredObjectsHaveStableDistinctReferences) {
  EXPECT_EQ(Model->argumentCount("ObReferenceObjectByName"), 8u);
  name("\\Driver\\NeverDDriver");
  auto R = query();
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0u);
  const auto Own = get(S + 0x300);
  EXPECT_EQ(Own, Model->driverObject());
  name("\\Driver\\pci");
  R = query();
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0u);
  const auto P = get(S + 0x300);
  EXPECT_NE(P, Own);
  EXPECT_EQ(get(P, 2), 4u);
  EXPECT_EQ(get(P + windows::DriverDeviceHead), 0u);
  R = query();
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(get(S + 0x300), P);
  EXPECT_EQ(call("ObfDereferenceObject", {P}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {P}), 0u);
  auto Underflow = Model->call("ObfDereferenceObject", {P});
  ASSERT_FALSE(bool(Underflow));
  llvm::consumeError(Underflow.takeError());
  EXPECT_EQ(call("ObfDereferenceObject", {Own}), 0u);
}

TEST_F(KernelDriverObject, UnknownInventoryNeverBecomesAnAbsentObject) {
  ok(initialize(options(false)));
  name("\\Driver\\pci");
  put(S + 0x300, 0x12345678);
  auto R = query();
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("requires driver_namespace"),
            std::string::npos);
  EXPECT_EQ(get(S + 0x300), 0x12345678u);
  name("\\Driver\\NeverDDriver");
  R = query();
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0u);
  EXPECT_EQ(get(S + 0x300), Model->driverObject());
  EXPECT_EQ(call("ObfDereferenceObject", {get(S + 0x300)}), 0u);
}

TEST_F(KernelDriverObject, CaseAndTypeFailuresClearOutputAfterLookup) {
  for (auto [Text, Flags, Type, Status] :
       {std::tuple{"\\DRIVER\\PCI", 0u, DT, uint32_t(0xc0000034)},
        std::tuple{"\\Driver\\missing", 0x240u, DT, uint32_t(0xc0000034)},
        std::tuple{"\\Driver\\pci", 0x240u,
                   object_type::identity(object_type::Kind::File),
                   uint32_t(0xc0000024)}}) {
    name(Text);
    put(S + 0x300, UINT64_MAX);
    auto R = query(Flags, Type);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(*R, Status);
    EXPECT_EQ(get(S + 0x300), 0u);
  }
  name("\\DRIVER\\PCI");
  auto R = query();
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {get(S + 0x300)}), 0u);
}

TEST_F(KernelDriverObject, EarlyPolicyFailuresPreserveOutputAndOwnership) {
  name("\\Driver\\pci");
  for (auto A : {std::array<uint64_t, 8>{S, 0x240, 1, 0, DT, 0, 0, S + 0x300},
                 std::array<uint64_t, 8>{S, 0x240, 0, 0, DT, 1, 0, S + 0x300},
                 std::array<uint64_t, 8>{S, 0x240, 0, 0, DT, 0, 1, S + 0x300},
                 std::array<uint64_t, 8>{S, 0x240, 0, 0, 0, 0, 0, S + 0x300},
                 std::array<uint64_t, 8>{S, 0x10, 0, 0, DT, 0, 0, S + 0x300}}) {
    put(S + 0x300, 0x87654321);
    auto R = Model->call("ObReferenceObjectByName", A);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(get(S + 0x300), 0x87654321u);
  }
  put(S, 1, 2);
  auto R = query();
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(get(S + 0x300), 0x87654321u);
}

TEST_F(KernelDriverObject,
       CapturedNameSurvivesOverlappingOutputAndTraceResets) {
  name("\\Driver\\pci");
  auto R = query(0x240, DT, S);
  ASSERT_TRUE(bool(R));
  EXPECT_EQ(*R, 0u);
  EXPECT_EQ(Model->callDetail(), "\\Driver\\pci");
  const auto P = get(S);
  EXPECT_NE(P, 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {P}), 0u);
  EXPECT_TRUE(Model->callDetail().empty());
}

TEST_F(KernelDriverObject, InvalidOutputDoesNotAcquireAnyReference) {
  name("\\Driver\\pci");
  ok(Memory->map(S + 0x10000, 4096, Read));
  auto R = query(0x240, DT, S + 0x10000);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  R = query();
  ASSERT_TRUE(bool(R));
  const auto P = get(S + 0x300);
  EXPECT_EQ(call("ObfDereferenceObject", {P}), 0u);
  R = Model->call("ObfDereferenceObject", {P});
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
}

TEST_F(KernelDriverObject, UnprovidedFieldsAndNameWritesRemainOpaque) {
  name("\\Driver\\pci");
  auto R = query();
  ASSERT_TRUE(bool(R));
  const auto P = get(S + 0x300);
  for (auto [A, N, W] :
       {std::tuple{P + windows::DriverNameOffset, 16u, false},
        std::tuple{P, 4u, false}, std::tuple{P + 8, 8u, false}})
    ok(Model->validateGuestAccess(A, N, W));
  const auto Buffer = get(P + windows::DriverNameOffset + 8);
  for (auto [A, N, W] :
       {std::tuple{P + windows::DriverStartOffset, 8u, false},
        std::tuple{P + 8, 8u, true}, std::tuple{Buffer, 2u, true}}) {
    auto E = Model->validateGuestAccess(A, N, W);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  EXPECT_EQ(call("ObfDereferenceObject", {P}), 0u);
}

TEST_F(KernelDriverObject, WritablePhysicalAliasesRetainObjectProtection) {
  auto O = options();
  O.PageTables = true;
  ok(initialize(O));
  name("\\Driver\\pci");
  auto R = query();
  ASSERT_TRUE(bool(R));
  const auto P = get(S + 0x300);
  ok(Model->synchronizeGuestPageTables());
  const auto PA = call("MmGetPhysicalAddress", {P});
  name("\\Device\\PhysicalMemory");
  put(S + 0x500, 48, 4);
  put(S + 0x510, S);
  put(S + 0x518, 0x240, 4);
  ASSERT_EQ(call("ZwOpenSection", {S + 0x600, 0xf001f, S + 0x500}), 0u);
  const auto H = get(S + 0x600);
  put(S + 0x620, 0);
  put(S + 0x628, 4096);
  put(S + 0x630, PA & ~uint64_t(4095));
  ASSERT_EQ(call("ZwMapViewOfSection", {H, UINT64_MAX, S + 0x620, 0, 0,
                                        S + 0x630, S + 0x628, 2, 0, 4}),
            0u);
  const auto V = get(S + 0x620), A = V + (PA & 4095);
  ok(Model->validateGuestAccess(A + 8, 8, false));
  EXPECT_EQ(get(A + 8), 0u);
  auto E = Model->validateGuestAccess(A + 8, 8, true);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  E = Model->validateGuestAccess(A + windows::DriverStartOffset, 8, false);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_EQ(call("ZwUnmapViewOfSection", {UINT64_MAX, V}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {P}), 0u);
}

TEST_F(KernelDriverObject, LiveReferencesPreventSuccessfulUnload) {
  name("\\Driver\\pci");
  auto R = query();
  ASSERT_TRUE(bool(R));
  const auto P = get(S + 0x300);
  put(Model->driverObject() + windows::DriverUnloadOffset, 0x180001000);
  ok(Model->finishEntry());
  auto U = Model->beginUnload();
  ASSERT_TRUE(bool(U));
  auto E = Model->finishUnload();
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("driver pointer references"),
            std::string::npos);
  EXPECT_EQ(call("ObfDereferenceObject", {P}), 0u);
  ok(Model->finishUnload());
  EXPECT_TRUE(Result.UnloadCompleted);
}

TEST_F(KernelDriverObject, UnsupportedVersionsAndHigherIRQLRemainExplicit) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    auto O = options();
    if (Mode == 0)
      O.Version.reset();
    if (Mode == 1)
      O.Version->Build = 22621;
    ok(initialize(O));
    name("\\Driver\\pci");
    if (Mode == 2)
      EXPECT_EQ(call("KfRaiseIrql", {1}), 0u);
    auto R = query();
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
}

std::filesystem::path namedFixture(const char *Name) {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + ".sys");
}
TEST(KernelDriverObjectCaller,
     EightArgumentClientBalancesReferencesAndUnloads) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      O.DriverNamespace = DriverObjectNamespace{{"\\Driver\\pci"}};
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(namedFixture("driver_object_names"), O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_EQ(R->NTStatus, 0u);
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      unsigned Queries = 0;
      for (const auto &C : R->Calls)
        if (C.Name == "ObReferenceObjectByName") {
          EXPECT_EQ(C.Arguments.size(), 8u);
          EXPECT_FALSE(C.Detail.empty());
          ++Queries;
        }
      EXPECT_EQ(Queries, 5u);
    }
}
TEST(KernelDriverObjectCaller, RealLeaksAndProviderWritesDoNotPass) {
  DriverOptions O;
  O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  O.Unload = true;
  O.DriverNamespace = DriverObjectNamespace{{"\\Driver\\pci"}};
  for (const char *Name :
       {"driver_object_names_leak", "driver_object_names_write"}) {
    auto R = emulateDriver(namedFixture(Name), O);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, DriverStopReason::ModelError) << R->Diagnostic;
    EXPECT_FALSE(R->UnloadCompleted);
    EXPECT_NE(R->Diagnostic.find("driver"), std::string::npos);
  }
}
} // namespace
} // namespace neverd::emulation
