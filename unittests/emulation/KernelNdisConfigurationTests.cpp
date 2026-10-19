///
/// \file
/// KernelNdisConfigurationTests.
///

#include "NdisConfigurationTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/WindowsUnicode.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "llvm/Support/FormatVariadic.h"

#include <cstdlib>
namespace neverd::emulation {
namespace {
class KernelNdisConfiguration : public testing::Test {
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
    ok(Memory->map(PC, 4096, Read | Execute));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = PC - 4096;
    I.Entry = PC;
    I.Size = 0x3000;
    ok(Model->initialize(I, Options));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void SetUp() override { initialize(test::ndisConfigurationOptions()); }
  llvm::Expected<uint64_t> scoped(llvm::StringRef Name,
                                  llvm::ArrayRef<uint64_t> A,
                                  llvm::StringRef Module = "ndis.sys") {
    KernelExportRegistry::Export E{Name.str(), Module.str(), 0};
    return Model->call(E, A, [](unsigned) -> llvm::Expected<uint64_t> {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unexpected argument");
    });
  }
  uint64_t nt(const char *N, std::initializer_list<uint64_t> A) {
    return value(Model->call(std::string(N), A));
  }
  uint64_t ndis(const char *N, std::initializer_list<uint64_t> A) {
    return value(scoped(N, A));
  }
  void put(uint64_t A, uint64_t V, unsigned W = 8) {
    ok(Memory->writeInteger(A, V, W));
  }
  uint64_t get(uint64_t A, unsigned W = 8) {
    ok(Model->validateGuestAccess(A, W, false));
    return value(Memory->readInteger(A, W));
  }
  void text(uint64_t A, llvm::StringRef T) {
    auto B = value(windows_unicode::bytes(T, true));
    ok(Memory->write(A + 0x100, B));
    put(A, B.size() - 2, 2);
    put(A + 2, B.size(), 2);
    put(A + 8, A + 0x100);
  }
  void config() {
    put(S + 0x1000, 0x001401a9, 4);
    put(S + 0x1008, test::NdisBinding);
    put(S + 0x1010, 0, 4);
  }
  uint64_t open() {
    config();
    EXPECT_EQ(ndis("NdisOpenConfigurationEx", {S + 0x1000, S + 8}), 0u);
    return get(S + 8);
  }
  uint64_t read(uint64_t H, llvm::StringRef Name, unsigned Type = 0) {
    text(S + 0x2000, Name);
    put(S, 0x1234, 4);
    put(S + 8, 0x11223344);
    ndis("NdisReadConfiguration", {S, S + 8, H, S + 0x2000, Type});
    EXPECT_EQ(get(S, 4), 0u);
    return get(S + 8);
  }
  uint64_t key() {
    text(S + 0x3000, test::NdisRegistry);
    put(S + 0x4000, 48, 4);
    put(S + 0x4008, 0);
    put(S + 0x4010, S + 0x3000);
    put(S + 0x4018, 0x240, 4);
    EXPECT_EQ(nt("ZwOpenKey", {S + 0x5000, 0xf003f, S + 0x4000}), 0u);
    return get(S + 0x5000);
  }
  void set(uint64_t K, uint32_t V) {
    text(S + 0x6000, "Answer");
    put(S + 0x7000, V, 4);
    EXPECT_EQ(nt("ZwSetValueKey", {K, S + 0x6000, 0, 4, S + 0x7000, 4}), 0u);
  }
};
TEST_F(KernelNdisConfiguration, NativeStringConversionRoutingAndTypedPayloads) {
  auto H = open();
  for (auto Requested : {0u, 1u, 2u, 3u, 4u, 0xffffffffu}) {
    auto P = read(H, "tExT", Requested);
    EXPECT_EQ(get(P, 4), Requested < 2 ? Requested : 2u);
    if (Requested < 2)
      EXPECT_EQ(get(P + 8, 4), Requested == 0 ? 42u : 66u);
    else {
      EXPECT_EQ(get(P + 8, 2), 4u);
      EXPECT_EQ(get(P + 10, 2), 6u);
      const auto B = get(P + 16);
      EXPECT_EQ(get(B, 2), '4');
      EXPECT_EQ(get(B + 2, 2), '2');
      // Returned public UNICODE_STRING can be consumed by the shared RTL owner.
      EXPECT_EQ(nt("RtlUnicodeStringToInteger", {P + 8, 10, S + 0x8000}), 0u);
      EXPECT_EQ(get(S + 0x8000, 4), 42u);
    }
  }
  auto P = read(H, "Answer", 4);
  EXPECT_EQ(get(P, 4), 0u);
  EXPECT_EQ(get(P + 8, 4), 42u);
  P = read(H, "Binary");
  EXPECT_EQ(get(P, 4), 4u);
  EXPECT_EQ(get(P + 8, 2), 3u);
  EXPECT_EQ(get(get(P + 16) + 2, 1), 3u);
  P = read(H, "Multi");
  EXPECT_EQ(get(P, 4), 3u);
  EXPECT_EQ(get(P + 8, 2), 10u);
  EXPECT_EQ(get(P + 10, 2), 10u);
  EXPECT_EQ(get(get(P + 16) + 4, 2), 'b');
  ndis("NdisCloseConfiguration", {H});
}
TEST_F(KernelNdisConfiguration,
       PublicParametersMatchTheOriginalNativeProducer) {
  struct Case {
    unsigned RegistryType;
    const char *Hex;
    unsigned Type, Length, Maximum, Integer;
  };
  const Case Cases[] = {
#define NEVERD_NDIS_PARAMETER_NATIVE_CASE(R, H, T, L, M, I) {R, H, T, L, M, I},
#include "NdisParameterNativeCases.def"
#undef NEVERD_NDIS_PARAMETER_NATIVE_CASE
  };
  ASSERT_EQ(std::size(Cases), 5u);
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Hex);
    const auto Bytes = llvm::fromHex(C.Hex);
    std::vector<uint8_t> Data(Bytes.begin(), Bytes.end());
    auto O = Options;
    O.Registry->at(0).Values = {{"ObservedValue", C.RegistryType, Data}};
    initialize(O);
    const auto H = open(), P = read(H, "ObservedValue", 2);
    EXPECT_EQ(get(P, 4), C.Type);
    if (!C.Type)
      EXPECT_EQ(get(P + 8, 4), C.Integer);
    else {
      EXPECT_EQ(get(P + 8, 2), C.Length);
      if (C.Type != 4)
        EXPECT_EQ(get(P + 10, 2), C.Maximum);
      std::vector<uint8_t> Observed(Data.size());
      ok(Memory->read(get(P + 16), Observed));
      EXPECT_EQ(Observed, Data);
    }
    ndis("NdisCloseConfiguration", {H});
    bad(Model->validateGuestAccess(P, 4, false), "freed");
  }
}
TEST_F(KernelNdisConfiguration,
       IndependentParametersExpireWithOnlyTheirOwningHandle) {
  auto A = open(), B = open();
  EXPECT_NE(A, B);
  auto P = read(A, "Answer"), Q = read(A, "Answer"), R = read(B, "Answer");
  EXPECT_NE(P, Q);
  EXPECT_NE(P, R);
  bad(Model->validateGuestAccess(A, 1, false), "opaque");
  bad(Model->validateGuestAccess(P, 4, true), "read-only");
  bad(Model->validateGuestAccess(P - 8, 8, false), "opaque");
  bad(Model->call("ExFreePool", {P - 8}), "NdisCloseConfiguration");
  ndis("NdisCloseConfiguration", {A});
  bad(Model->validateGuestAccess(P, 4, false), "freed");
  bad(Model->validateGuestAccess(Q, 4, false), "freed");
  EXPECT_EQ(get(R + 8, 4), 42u);
  bad(scoped("NdisCloseConfiguration", {A}), "live owned");
  ndis("NdisCloseConfiguration", {B});
}
TEST_F(KernelNdisConfiguration, PredefinedObservationsRemainStableAfterClose) {
  auto H = open();
  auto P = read(H, "ProcessorType", 4);
  EXPECT_EQ(get(P, 4), 0u);
  EXPECT_EQ(get(P + 8, 4), 4u);
  auto E = read(H, "Environment");
  EXPECT_EQ(get(E + 8, 4), 1u);
  auto V = read(H, "NdisVersion");
  EXPECT_EQ(get(V + 8, 4), 0x60054u);
  ndis("NdisCloseConfiguration", {H});
  EXPECT_EQ(get(P + 8, 4), 4u);
  EXPECT_EQ(get(E + 8, 4), 1u);
  EXPECT_EQ(get(V + 8, 4), 0x60054u);
  H = open();
  EXPECT_EQ(read(H, "processortype", 0xffffffff), P);
  ndis("NdisCloseConfiguration", {H});
  auto O = Options;
  O.NdisConfiguration->Version.reset();
  initialize(O);
  H = open();
  text(S + 0x2000, "NdisVersion");
  bad(scoped("NdisReadConfiguration", {S, S + 8, H, S + 0x2000, 0}),
      "explicit observed NDIS version");
  ndis("NdisCloseConfiguration", {H});
}
TEST_F(KernelNdisConfiguration,
       ReadsShareLiveRegistryAndPreservePreviousSnapshots) {
  auto H = open(), K = key();
  auto P = read(H, "Answer");
  set(K, 99);
  auto Q = read(H, "Answer");
  EXPECT_EQ(get(P + 8, 4), 42u);
  EXPECT_EQ(get(Q + 8, 4), 99u);
  text(S + 0x6000, "Answer");
  EXPECT_EQ(nt("ZwDeleteValueKey", {K, S + 0x6000}), 0u);
  put(S + 8, 0x12345678);
  text(S + 0x2000, "Answer");
  ndis("NdisReadConfiguration", {S, S + 8, H, S + 0x2000, 0});
  EXPECT_EQ(get(S, 4), 0xc0000001u);
  EXPECT_EQ(get(S + 8), 0x12345678u);
  nt("ZwClose", {K});
  ndis("NdisCloseConfiguration", {H});
}
TEST_F(KernelNdisConfiguration,
       KeyDeletionAndRecreationCannotRebindAnExistingConfiguration) {
  auto H = open(), K = key();
  read(H, "Answer");
  EXPECT_EQ(nt("ZwDeleteKey", {K}), 0u);
  nt("ZwClose", {K});
  EXPECT_EQ(nt("ZwCreateKey", {S + 0x5000, 0xf003f, S + 0x4000, 0, 0, 0, 0}),
            0u);
  K = get(S + 0x5000);
  set(K, 7);
  text(S + 0x2000, "Answer");
  put(S + 8, 0x9876);
  ndis("NdisReadConfiguration", {S, S + 8, H, S + 0x2000, 0});
  EXPECT_EQ(get(S, 4), 0xc0000001u);
  EXPECT_EQ(get(S + 8), 0x9876u);
  ndis("NdisCloseConfiguration", {H});
  H = open();
  auto P = read(H, "Answer");
  EXPECT_EQ(get(P + 8, 4), 7u);
  ndis("NdisCloseConfiguration", {H});
  nt("ZwClose", {K});
}
TEST_F(KernelNdisConfiguration, HeaderChecksProvidersAndNamespacesFailClearly) {
  for (auto Header : {0x001301a9u, 0x001400a9u, 0x001401a8u}) {
    config();
    put(S + 0x1000, Header, 4);
    put(S + 8, 0x1234);
    EXPECT_EQ(ndis("NdisOpenConfigurationEx", {S + 0x1000, S + 8}),
              0xc000000du);
    EXPECT_EQ(get(S + 8), 0x1234u);
  }
  config();
  put(S + 0x1000, 0x001402a9, 4);
  EXPECT_EQ(ndis("NdisOpenConfigurationEx", {S + 0x1000, S + 8}), 0u);
  ndis("NdisCloseConfiguration", {get(S + 8)});
  config();
  put(S + 0x1010, 1, 4);
  bad(scoped("NdisOpenConfigurationEx", {S + 0x1000, S + 8}),
      "instance-specific");
  config();
  put(S + 0x1008, test::NdisBinding + 8);
  bad(scoped("NdisOpenConfigurationEx", {S + 0x1000, S + 8}),
      "unknown external");
  auto O = Options;
  O.NdisConfiguration.reset();
  initialize(O);
  config();
  bad(scoped("NdisOpenConfigurationEx", {S + 0x1000, S + 8}),
      "explicit ndis_configuration");
  for (const char *Name : {"NdisOpenConfigurationEx", "NdisReadConfiguration",
                           "NdisCloseConfiguration"}) {
    KernelExportRegistry::Export E{Name, "ntoskrnl.exe", 0};
    EXPECT_FALSE(KernelModel::argumentCount(E));
    E.Module = "ndis.sys";
    EXPECT_TRUE(KernelModel::argumentCount(E));
    bad(Model->call(Name, {}), "unknown API");
    bad(scoped(Name, {}), "arity");
  }
}
TEST_F(KernelNdisConfiguration,
       IrqlFiltersAndBadOutputPreflightPreserveOwnership) {
  auto H = open(), P = read(H, "Answer");
  nt("KfRaiseIrql", {2});
  EXPECT_EQ(get(P + 8, 4), 42u);
  bad(scoped("NdisCloseConfiguration", {H}), "IRQL");
  nt("KeLowerIrql", {0});
  nt("KfRaiseIrql", {1});
  bad(scoped("NdisReadConfiguration", {S, S + 8, H, S + 0x2000, 0}), "IRQL");
  bad(scoped("NdisOpenConfigurationEx", {S + 0x1000, S + 8}), "IRQL");
  nt("KeLowerIrql", {0});
  auto F = scoped("NdisReadConfiguration", {S, 0, H, S + 0x2000, 0});
  ASSERT_FALSE(bool(F));
  llvm::consumeError(F.takeError());
  EXPECT_EQ(nt("CmRegisterCallback", {PC, 0, S + 0x8000}), 0u);
  bad(scoped("NdisReadConfiguration", {S, S + 8, H, S + 0x2000, 0}),
      "filter continuations");
  bad(scoped("NdisOpenConfigurationEx", {S + 0x1000, S + 8}),
      "filter continuations");
  nt("CmUnRegisterCallback", {get(S + 0x8000)});
  EXPECT_EQ(read(H, "Answer") != 0, true);
  ndis("NdisCloseConfiguration", {H});
}
TEST_F(KernelNdisConfiguration,
       ResourceFailureRetainsNativeStatusAndOutputRules) {
  auto H = open();
  std::vector<uint64_t> Fillers;
  for (auto Size : {0x10000u, 0x1000u, 0x100u, 0x10u})
    for (unsigned I = 0; I < 256; ++I) {
      auto P = nt("ExAllocatePoolWithTag", {0x200, Size, 0x12345678});
      if (!P)
        break;
      Fillers.push_back(P);
      ASSERT_LT(I, 255u);
    }
  config();
  put(S + 8, 0x1234);
  EXPECT_EQ(ndis("NdisOpenConfigurationEx", {S + 0x1000, S + 8}), 0xc000009au);
  EXPECT_EQ(get(S + 8), 0x1234u);
  text(S + 0x2000, "Text");
  put(S + 8, 0x5678);
  ndis("NdisReadConfiguration", {S, S + 8, H, S + 0x2000, 0});
  EXPECT_EQ(get(S, 4), 0xc0000001u);
  EXPECT_EQ(get(S + 8), 0x5678u);
  auto P = read(H, "ProcessorType");
  EXPECT_EQ(get(P + 8, 4), 4u);
  for (auto F : Fillers)
    nt("ExFreePool", {F});
  ndis("NdisCloseConfiguration", {H});
}
TEST_F(KernelNdisConfiguration, UnloadRequiresClosingConfigurationHandles) {
  auto H = open();
  read(H, "Text", 2);
  put(Model->driverObject() + windows::DriverUnloadOffset, PC);
  ok(Model->finishEntry());
  auto U = Model->beginUnload();
  ASSERT_TRUE(bool(U));
  bad(Model->finishUnload(), "NDIS configuration handles");
  ndis("NdisCloseConfiguration", {H});
  ok(Model->finishUnload());
}
TEST(NdisConfigurationScenario,
     NativeAndJsonValidationShareBindingIdentityRules) {
  auto O = test::ndisConfigurationOptions();
  auto E = validateDriverNdisConfiguration(O);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  auto Reject = [](DriverOptions O) {
    auto E = validateDriverNdisConfiguration(O);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  };
  auto Bad = O;
  Bad.Registry.reset();
  Reject(Bad);
  Bad = O;
  Bad.Version.reset();
  Reject(Bad);
  for (auto H : {0ULL, 1ULL, 0x0000800000000000ULL, 0xffffa00000000001ULL}) {
    Bad = O;
    Bad.NdisConfiguration->Bindings[0].Handle = H;
    Reject(Bad);
  }
  Bad = O;
  Bad.NdisConfiguration->Bindings.push_back(Bad.NdisConfiguration->Bindings[0]);
  Reject(Bad);
  Bad = O;
  Bad.NdisConfiguration->Version = 0x70000;
  Reject(Bad);
  Bad = O;
  Bad.NdisConfiguration->Bindings[0].RegistryPath = "\\Registry\\Machine";
  Reject(Bad);
  auto J = test::ndisConfigurationScenario();
  auto Parsed = driverOptionsFromScenarioJSON(
      llvm::formatv("{0}", llvm::json::Value(std::move(J))).str());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_TRUE(Parsed->NdisConfiguration);
  EXPECT_EQ(Parsed->NdisConfiguration->Bindings[0].Handle, test::NdisBinding);
  DriverResult R;
  R.Configuration = *Parsed;
  auto Report = llvm::json::parse(driverResultJSON(R));
  ASSERT_TRUE(bool(Report));
  const auto *C = Report->getAsObject()
                      ->getObject("configuration")
                      ->getObject("ndis_configuration");
  ASSERT_NE(C, nullptr);
  EXPECT_EQ(C->getInteger("version"), 0x60054);
  EXPECT_EQ(
      (*C->getArray("bindings"))[0].getAsObject()->get("handle")->getAsUINT64(),
      test::NdisBinding);
  for (const char *V :
       {"null", "[]", "{}", "{\"bindings\":[],\"version\":null}",
        "{\"bindings\":[{\"handle\":1}]}", "{\"bindings\":[],\"typo\":1}"}) {
    auto P = driverOptionsFromScenarioJSON(
        std::string("{\"ndis_configuration\":") + V + "}");
    ASSERT_FALSE(bool(P));
    llvm::consumeError(P.takeError());
  }
}
void checkNdisDriver(DriverOptions O) {
  auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                             "driver_ndis_configuration.sys",
                         O);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
  EXPECT_EQ(R->NTStatus, 0u);
  EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
  EXPECT_EQ(R->Messages,
            (std::vector<std::string>{"ndis configuration lifetime passed",
                                      "ndis configuration unload passed"}));
  unsigned Calls = 0;
  for (const auto &C : R->Calls)
    if (llvm::StringRef(C.Name).starts_with("Ndis"))
      ++Calls;
  EXPECT_EQ(Calls, 15u);
}
TEST(NdisConfigurationDriver,
     CompiledClientUsesScopedImportsAndClosesEveryOwner) {
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (bool Paging : {false, true}) {
        auto O = test::ndisConfigurationOptions();
        O.Contract = C;
        O.PageTables = Paging;
        if (Preemptive)
          O.Scheduling = DriverScheduling{23, 1};
        checkNdisDriver(O);
      }
  auto I = loadDriverImage(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                               "driver_ndis_configuration.sys",
                           64 * 1024 * 1024);
  ASSERT_TRUE(bool(I));
  unsigned Count = 0;
  for (const auto &F : I->Imports)
    if (llvm::StringRef(F.Name).starts_with("Ndis")) {
      EXPECT_EQ(F.Module, "ndis.sys");
      ++Count;
    }
  EXPECT_EQ(Count, 3u);
}
TEST(NdisConfigurationDriver,
     ExplicitNtReferenceKeepsTheNdisNamespaceSeparate) {
  auto *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE"),
       *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  for (auto C : {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      auto O = test::ndisConfigurationOptions();
      O.Contract = C;
      O.KernelImage = Image;
      O.KernelPDB = PDB;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      checkNdisDriver(O);
    }
}
} // namespace
} // namespace neverd::emulation
