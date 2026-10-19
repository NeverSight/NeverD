///
/// \file
/// KernelNetioTests.
///

#include "NetworkInterfacesTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstdlib>
namespace neverd::emulation {
namespace {
class KernelNetio : public testing::Test {
protected:
  static constexpr uint64_t S = 0x22000000, PC = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverOptions Options;
  DriverResult Result;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <class T> T value(llvm::Expected<T> V) {
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return {};
    }
    return std::move(*V);
  }
  template <class T> void bad(llvm::Expected<T> V, llvm::StringRef Text) {
    ASSERT_FALSE(bool(V));
    auto M = llvm::toString(V.takeError());
    EXPECT_NE(M.find(Text.str()), std::string::npos) << M;
  }
  void bad(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    auto M = llvm::toString(std::move(E));
    EXPECT_NE(M.find(Text.str()), std::string::npos) << M;
  }
  void initialize(DriverOptions O) {
    Model.reset();
    Result = {};
    Options = std::move(O);
    Memory = value(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(S, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = PC - 4096;
    I.Entry = PC;
    I.Size = 0x3000;
    ok(Model->initialize(I, Options));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void SetUp() override { initialize(test::networkInterfacesOptions()); }
  llvm::Expected<uint64_t> scoped(llvm::StringRef N, llvm::ArrayRef<uint64_t> A,
                                  llvm::StringRef M = "netio.sys") {
    KernelExportRegistry::Export E{N.str(), M.str(), 0};
    return Model->call(E, A, [](unsigned) -> llvm::Expected<uint64_t> {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "extra argument");
    });
  }
  uint64_t nt(const char *N, std::initializer_list<uint64_t> A) {
    return value(Model->call(std::string(N), A));
  }
  uint64_t net(const char *N, std::initializer_list<uint64_t> A) {
    return value(scoped(N, A));
  }
  void put(uint64_t A, uint64_t V, unsigned W = 8) {
    ok(Memory->writeInteger(A, V, W));
  }
  uint64_t get(uint64_t A, unsigned W = 8) {
    ok(Model->validateGuestAccess(A, W, false));
    return value(Memory->readInteger(A, W));
  }
  uint64_t query(uint64_t L = 0) {
    EXPECT_EQ(net("GetIfTable2Ex", {L, S}), 0u);
    return get(S);
  }
};
TEST_F(KernelNetio,
       EachLevelCopiesCompleteObservedRowsAndNativeAllocationLayout) {
  auto O = Options;
  std::reverse(O.NetworkInterfaces->Levels[1]->begin(),
               O.NetworkInterfaces->Levels[1]->end());
  initialize(O);
  for (unsigned L = 0; L < 3; ++L) {
    const auto P = query(L);
    ASSERT_NE(P, 0u);
    EXPECT_EQ(get(P, 4), 2u);
    EXPECT_EQ(get(P + 4, 4), 0u);
    EXPECT_EQ(get(P + 8 + 2 * DriverNetworkRowSize, 4), 0u);
    for (unsigned I = 0; I < 2; ++I) {
      std::vector<uint8_t> Bytes(DriverNetworkRowSize);
      ok(Memory->read(P + 8 + I * DriverNetworkRowSize, Bytes));
      EXPECT_EQ(Bytes, O.NetworkInterfaces->Levels[L]->at(I));
    }
    net("FreeMibTable", {P});
    bad(Model->validateGuestAccess(P, 1, false), "freed");
  }
  initialize(test::networkInterfacesOptions(0));
  const auto Empty = query();
  EXPECT_EQ(get(Empty, 8), 0u);
  EXPECT_EQ(get(Empty + 8, 4), 0u);
  net("FreeMibTable", {Empty});
}
TEST_F(KernelNetio, FreshCopiesOwnIndependentMutableNonpagedBuffers) {
  const auto A = query(), B = query();
  EXPECT_NE(A, B);
  put(A + 8 + 0x424, 0x77, 1);
  EXPECT_EQ(get(B + 8 + 0x424, 1), 2u);
  const auto C = query();
  EXPECT_EQ(get(C + 8 + 0x424, 1), 2u);
  nt("KfRaiseIrql", {2});
  EXPECT_EQ(get(A + 8 + 0x424, 1), 0x77u);
  bad(scoped("FreeMibTable", {A}), "IRQL");
  nt("KeLowerIrql", {0});
  nt("KfRaiseIrql", {1});
  net("FreeMibTable", {A});
  bad(scoped("GetIfTable2Ex", {0, S}), "IRQL");
  nt("KeLowerIrql", {0});
  net("FreeMibTable", {B});
  net("FreeMibTable", {C});
}
TEST_F(KernelNetio, MissingFactsAndInvalidLevelsDoNotInventOutputs) {
  put(S, 0x1234);
  EXPECT_EQ(net("GetIfTable2Ex", {3, S}), 0xc000000du);
  EXPECT_EQ(get(S), 0x1234u);
  EXPECT_EQ(net("GetIfTable2Ex", {0xffffffff, 0}), 0xc000000du);
  const auto P = query(0x100000001ULL);
  EXPECT_EQ(get(P + 8 + 0x484, 4), 1u);
  net("FreeMibTable", {P});
  auto O = Options;
  O.NetworkInterfaces->Levels[1].reset();
  initialize(O);
  put(S, 0x4321);
  bad(scoped("GetIfTable2Ex", {1, S}), "explicit network_interfaces");
  EXPECT_EQ(get(S), 0x4321u);
  O.NetworkInterfaces.reset();
  initialize(O);
  put(S, 0x8765);
  bad(scoped("GetIfTable2Ex", {0, S}), "explicit network_interfaces");
  EXPECT_EQ(get(S), 0x8765u);
}
TEST_F(KernelNetio, ScopedArityAndInvalidOutputAreRejectedBeforeAllocation) {
  KernelExportRegistry::Export E{"GetIfTable2Ex", "netio.sys", 0};
  EXPECT_EQ(KernelModel::argumentCount(E), 2u);
  for (auto M : {"ntoskrnl.exe", "ndis.sys", "fltmgr.sys"}) {
    E.Module = M;
    EXPECT_FALSE(KernelModel::argumentCount(E));
    auto Failed = scoped("GetIfTable2Ex", {0, S}, M);
    ASSERT_FALSE(bool(Failed));
    llvm::consumeError(Failed.takeError());
  }
  bad(scoped("GetIfTable2Ex", {0}), "arity");
  bad(Model->call("GetIfTable2Ex", {0, S}), "unknown API");
  auto Failed = scoped("GetIfTable2Ex", {0, 0});
  ASSERT_FALSE(bool(Failed));
  llvm::consumeError(Failed.takeError());
  const auto P = query();
  net("FreeMibTable", {P});
}
TEST_F(KernelNetio,
       ReleaseRequiresTheExactOwnedTableAndGenericPoolFreeRetiresIt) {
  const auto P = query();
  bad(scoped("FreeMibTable", {P + 8}), "owned live MIB");
  const auto Other = nt("ExAllocatePoolWithTag", {0x200, 32, 0x70614c4e});
  bad(scoped("FreeMibTable", {Other}), "owned live MIB");
  nt("ExFreePoolWithTag", {Other, 0x70614c4e});
  nt("ExFreePool", {P});
  bad(scoped("FreeMibTable", {P}), "owned live MIB");
  const auto Q = query();
  net("FreeMibTable", {Q});
  bad(scoped("FreeMibTable", {Q}), "owned live MIB");
  bad(scoped("FreeMibTable", {0}), "owned live MIB");
}
TEST_F(KernelNetio, NativeOutOfMemoryStatusClearsTheOutputAndLeavesNoTable) {
  initialize(test::networkInterfacesOptions(64));
  std::vector<uint64_t> Fillers;
  for (unsigned I = 0; I < 256; ++I) {
    const auto P = nt("ExAllocatePoolWithTag", {0x200, 0x10000, 0x12345678});
    if (!P)
      break;
    Fillers.push_back(P);
  }
  ASSERT_FALSE(Fillers.empty());
  ASSERT_LT(Fillers.size(), 256u);
  put(S, 0x1234);
  EXPECT_EQ(net("GetIfTable2Ex", {0, S}), 0xc0000017u);
  EXPECT_EQ(get(S), 0u);
  for (auto P : Fillers)
    nt("ExFreePoolWithTag", {P, 0x12345678});
}
TEST_F(KernelNetio, UnloadRejectsOutstandingTableOwnership) {
  const auto P = query();
  put(Model->driverObject() + windows::DriverUnloadOffset, PC);
  ok(Model->finishEntry());
  auto U = Model->beginUnload();
  ASSERT_TRUE(bool(U));
  bad(Model->finishUnload(), "allocations");
  net("FreeMibTable", {P});
  ok(Model->finishUnload());
}
TEST(NetworkInterfacesScenario,
     SharedValidationRejectsContradictoryAndMalformedRows) {
  auto Valid = test::networkInterfacesOptions();
  auto Accept = validateDriverNetworkInterfaces(Valid);
  ASSERT_FALSE(bool(Accept)) << llvm::toString(std::move(Accept));
  const auto Reject = [](const DriverOptions &O) {
    auto E = validateDriverNetworkInterfaces(O);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  };
  auto O = Valid;
  O.Version.reset();
  Reject(O);
  O = Valid;
  O.NetworkInterfaces.emplace();
  Reject(O);
  O = Valid;
  O.NetworkInterfaces->Levels[0]->at(0).pop_back();
  Reject(O);
  O = Valid;
  O.NetworkInterfaces->Levels[0]->push_back(
      O.NetworkInterfaces->Levels[0]->at(0));
  Reject(O);
  for (unsigned At : {0u, 8u, 12u, 0x420u, 0x468u, 0x470u, 0x474u, 0x478u,
                      0x47cu, 0x484u, 0x488u, 0x48cu, 0x4a0u}) {
    O = Valid;
    llvm::support::endian::write32le(
        O.NetworkInterfaces->Levels[0]->at(0).data() + At, 0xffffffff);
    Reject(O);
  }
  O = Valid;
  O.NetworkInterfaces->Levels[0]->at(0)[0x1c] = 0x00;
  O.NetworkInterfaces->Levels[0]->at(0)[0x1d] = 0xd8;
  Reject(O);
  O = Valid;
  for (unsigned I = 0; I < 257; ++I)
    llvm::support::endian::write16le(
        O.NetworkInterfaces->Levels[0]->at(0).data() + 0x1c + 2 * I, 'x');
  Reject(O);
  O = Valid;
  O.NetworkInterfaces->Levels[1]->clear();
  Reject(O);
  O = Valid;
  O.NetworkInterfaces->Levels[1]->at(0)[12] ^= 1;
  Reject(O);
}
TEST(NetworkInterfacesScenario,
     JsonRoundTripPreservesEveryByteAndAbsentLevels) {
  auto J = test::networkInterfacesScenario();
  auto *N = J.getObject("network_interfaces");
  N->erase("raw");
  auto O = driverOptionsFromScenarioJSON(
      llvm::formatv("{0}", llvm::json::Value(std::move(J))).str());
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->NetworkInterfaces);
  EXPECT_FALSE(O->NetworkInterfaces->Levels[1]);
  EXPECT_EQ(O->NetworkInterfaces->Levels[0]->at(0), test::networkRow(1, 0));
  DriverResult R;
  R.Configuration = *O;
  auto Report = llvm::json::parse(driverResultJSON(R));
  ASSERT_TRUE(bool(Report));
  const auto *Serialized = Report->getAsObject()
                               ->getObject("configuration")
                               ->getObject("network_interfaces");
  ASSERT_NE(Serialized, nullptr);
  EXPECT_EQ(Serialized->getArray("normal")->size(), 2u);
  EXPECT_EQ((*Serialized->getArray("normal"))[0].getAsString(),
            llvm::toHex(test::networkRow(1, 0), true));
  EXPECT_EQ(Serialized->get("raw"), nullptr);
  for (const char *Bad : {"null", "[]", "{}", "{\"normal\":null}",
                          "{\"other\":[]}", "{\"normal\":[\"00\"]}"}) {
    auto P = driverOptionsFromScenarioJSON(
        std::string("{\"windows_version\":{\"major\":10,\"minor\":0,\"build\":"
                    "19041},\"network_interfaces\":") +
        Bad + "}");
    ASSERT_FALSE(bool(P));
    llvm::consumeError(P.takeError());
  }
}
void checkNetioDriver(DriverOptions O) {
  auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                             "driver_netio_interfaces.sys",
                         O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
  EXPECT_EQ(R->NTStatus, 0u);
  EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
  EXPECT_EQ(R->Messages,
            (std::vector<std::string>{"netio interface observations passed",
                                      "netio unload passed"}));
  unsigned Calls = 0;
  for (const auto &C : R->Calls)
    if (C.Name == "GetIfTable2Ex" || C.Name == "FreeMibTable")
      ++Calls;
  EXPECT_EQ(Calls, 13u);
}
TEST(NetioDriver, CompiledImportsPreserveAllLevelsAndCleanUnload) {
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Paging : {false, true}) {
        auto O = test::networkInterfacesOptions();
        O.Contract = C;
        O.PageTables = Paging;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        checkNetioDriver(O);
      }
  auto I = loadDriverImage(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_netio_interfaces.sys",
                           64 * 1024 * 1024);
  ASSERT_TRUE(bool(I));
  unsigned Count = 0;
  for (const auto &F : I->Imports)
    if (F.Name == "GetIfTable2Ex" || F.Name == "FreeMibTable") {
      EXPECT_EQ(F.Module, "netio.sys");
      ++Count;
    }
  EXPECT_EQ(Count, 2u);
}
TEST(NetioDriver, ExplicitKernelReferenceKeepsNetioImportsInTheirOwnNamespace) {
  const auto *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
             *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      auto O = test::networkInterfacesOptions();
      O.Contract = C;
      O.KernelImage = Image;
      O.KernelPDB = PDB;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      checkNetioDriver(O);
    }
}
} // namespace
} // namespace neverd::emulation
