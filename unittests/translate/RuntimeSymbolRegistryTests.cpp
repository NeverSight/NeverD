//===- RuntimeSymbolRegistryTests.cpp - Runtime symbol boundary tests ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/translate/RuntimeSymbolRegistry.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::translate;

namespace {

RuntimeSymbolRegistryErrorCode takeReason(llvm::Error Error) {
  RuntimeSymbolRegistryErrorCode Reason =
      static_cast<RuntimeSymbolRegistryErrorCode>(
          std::numeric_limits<uint8_t>::max());
  bool Handled = false;
  llvm::handleAllErrors(std::move(Error),
                        [&](const RuntimeSymbolRegistryError &RegistryError) {
                          Handled = true;
                          Reason = RegistryError.reason();
                        });
  EXPECT_TRUE(Handled);
  return Reason;
}

template <typename T>
RuntimeSymbolRegistryErrorCode takeReason(llvm::Expected<T> Value) {
  if (Value) {
    ADD_FAILURE() << "expected a runtime symbol registry error";
    return static_cast<RuntimeSymbolRegistryErrorCode>(
        std::numeric_limits<uint8_t>::max());
  }
  return takeReason(Value.takeError());
}

llvm::orc::ExecutorAddr addressOf(const RuntimeABIHelperBindingV1 &Binding) {
  if (Binding.Class == RuntimeABIHelperClassV1::Load)
    return llvm::orc::ExecutorAddr::fromPtr(Binding.Load);
  return llvm::orc::ExecutorAddr::fromPtr(Binding.Store);
}

uint32_t alternateLoad(void *, uint64_t, uint32_t) noexcept { return 0; }

TEST(RuntimeSymbolRegistry, ResolvesOnlyExactRegisteredNames) {
  RuntimeSymbolRegistryV1 Registry =
      llvm::cantFail(RuntimeSymbolRegistryV1::create());
  const RuntimeABIHelperBindingV1 &Binding =
      runtimeABIHelperBindingsV1().front();

  llvm::Expected<llvm::orc::ExecutorAddr> Address =
      Registry.lookup(Binding.Name);
  ASSERT_TRUE(static_cast<bool>(Address));
  EXPECT_EQ(*Address, addressOf(Binding));

  EXPECT_EQ(takeReason(Registry.lookup(Binding.Name.drop_back())),
            RuntimeSymbolRegistryErrorCode::UnknownSymbol);
  EXPECT_EQ(takeReason(Registry.lookup(Binding.Name.str() + "_suffix")),
            RuntimeSymbolRegistryErrorCode::UnknownSymbol);
  EXPECT_EQ(takeReason(Registry.lookup("NVD_RT_V1_LOAD8_LE")),
            RuntimeSymbolRegistryErrorCode::UnknownSymbol);
}

TEST(RuntimeSymbolRegistry, OwnsSortedEntriesAndArtifactAllowlistViews) {
  RuntimeSymbolRegistryV1 Registry =
      llvm::cantFail(RuntimeSymbolRegistryV1::create());
  const std::vector<llvm::StringRef> Names = Registry.names();
  const std::vector<llvm::StringRef> Allowlist =
      Registry.artifactVerifierAllowlist();

  ASSERT_EQ(Names.size(), runtimeABIHelperBindingsV1().size());
  ASSERT_EQ(Names.size(), Registry.entries().size());
  EXPECT_TRUE(std::is_sorted(Names.begin(), Names.end()));
  EXPECT_EQ(Names, Allowlist);
  for (size_t Index = 0; Index != Names.size(); ++Index)
    EXPECT_EQ(Names[Index], Registry.entries()[Index].name());
}

TEST(RuntimeSymbolRegistry, IdentityIsVersionedAddressFreeAndOrderStable) {
  const llvm::ArrayRef<RuntimeABIHelperBindingV1> Builtins =
      runtimeABIHelperBindingsV1();
  llvm::SmallVector<RuntimeABIHelperBindingV1, 8> Reversed(Builtins.begin(),
                                                           Builtins.end());
  std::reverse(Reversed.begin(), Reversed.end());

  RuntimeSymbolRegistryV1 Default =
      llvm::cantFail(RuntimeSymbolRegistryV1::create(Builtins));
  RuntimeSymbolRegistryV1 Reordered =
      llvm::cantFail(RuntimeSymbolRegistryV1::create(Reversed));
  EXPECT_TRUE(
      Default.identity().starts_with("nvd-runtime-symbol-registry-v1-sha256:"));
  EXPECT_EQ(Default.identity(), Reordered.identity());

  Reversed.back().Load = &alternateLoad;
  RuntimeSymbolRegistryV1 AlternateAddress =
      llvm::cantFail(RuntimeSymbolRegistryV1::create(Reversed));
  EXPECT_EQ(Default.identity(), AlternateAddress.identity());
  EXPECT_NE(llvm::cantFail(AlternateAddress.lookup(Reversed.back().Name)),
            llvm::cantFail(Default.lookup(Reversed.back().Name)));
}

TEST(RuntimeSymbolRegistry, PreservesEveryHelperClassAndNativeAddress) {
  RuntimeSymbolRegistryV1 Registry =
      llvm::cantFail(RuntimeSymbolRegistryV1::create());
  for (const RuntimeABIHelperBindingV1 &Binding :
       runtimeABIHelperBindingsV1()) {
    llvm::Expected<llvm::orc::ExecutorAddr> Address =
        Registry.lookup(Binding.Name);
    ASSERT_TRUE(static_cast<bool>(Address));
    EXPECT_EQ(*Address, addressOf(Binding));

    const auto Entry =
        std::find_if(Registry.entries().begin(), Registry.entries().end(),
                     [&](const RuntimeSymbolEntryV1 &Candidate) {
                       return Candidate.name() == Binding.Name;
                     });
    ASSERT_NE(Entry, Registry.entries().end());
    EXPECT_EQ(Entry->helperClass(), Binding.Class);
    EXPECT_EQ(Entry->address(), addressOf(Binding));
  }
}

TEST(RuntimeSymbolRegistry, RejectsDuplicateConflictingAndNullBindings) {
  const llvm::ArrayRef<RuntimeABIHelperBindingV1> Builtins =
      runtimeABIHelperBindingsV1();

  llvm::SmallVector<RuntimeABIHelperBindingV1, 9> Duplicate(Builtins.begin(),
                                                            Builtins.end());
  Duplicate.push_back(Duplicate.front());
  EXPECT_EQ(takeReason(RuntimeSymbolRegistryV1::create(Duplicate)),
            RuntimeSymbolRegistryErrorCode::DuplicateName);

  llvm::SmallVector<RuntimeABIHelperBindingV1, 8> WrongClass(Builtins.begin(),
                                                             Builtins.end());
  WrongClass.front().Class = RuntimeABIHelperClassV1::Store;
  EXPECT_EQ(takeReason(RuntimeSymbolRegistryV1::create(WrongClass)),
            RuntimeSymbolRegistryErrorCode::HelperClassMismatch);

  llvm::SmallVector<RuntimeABIHelperBindingV1, 8> Conflicting(Builtins.begin(),
                                                              Builtins.end());
  Conflicting.front().Store = &nvd_rt_v1_store8_le;
  EXPECT_EQ(takeReason(RuntimeSymbolRegistryV1::create(Conflicting)),
            RuntimeSymbolRegistryErrorCode::InvalidFunctionPointers);

  llvm::SmallVector<RuntimeABIHelperBindingV1, 8> Null(Builtins.begin(),
                                                       Builtins.end());
  Null.front().Load = nullptr;
  EXPECT_EQ(takeReason(RuntimeSymbolRegistryV1::create(Null)),
            RuntimeSymbolRegistryErrorCode::NullAddress);
}

TEST(RuntimeSymbolRegistry, RejectsNonCanonicalUnknownAndIncompleteBindings) {
  const llvm::ArrayRef<RuntimeABIHelperBindingV1> Builtins =
      runtimeABIHelperBindingsV1();

  llvm::SmallVector<RuntimeABIHelperBindingV1, 8> InvalidName(Builtins.begin(),
                                                              Builtins.end());
  InvalidName.front().Name = "nvd_rt_v1_Load8_le";
  EXPECT_EQ(takeReason(RuntimeSymbolRegistryV1::create(InvalidName)),
            RuntimeSymbolRegistryErrorCode::InvalidName);

  llvm::SmallVector<RuntimeABIHelperBindingV1, 8> Unknown(Builtins.begin(),
                                                          Builtins.end());
  Unknown.front().Name = "nvd_rt_v1_unknown";
  EXPECT_EQ(takeReason(RuntimeSymbolRegistryV1::create(Unknown)),
            RuntimeSymbolRegistryErrorCode::BindingNotInABI);

  llvm::SmallVector<RuntimeABIHelperBindingV1, 8> Missing(Builtins.begin(),
                                                          Builtins.end());
  Missing.pop_back();
  EXPECT_EQ(takeReason(RuntimeSymbolRegistryV1::create(Missing)),
            RuntimeSymbolRegistryErrorCode::MissingBinding);
}

} // namespace
