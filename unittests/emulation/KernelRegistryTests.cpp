//===- KernelRegistryTests.cpp - Registry API contracts -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Concrete registry state, handle capabilities, and guest buffer contracts.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/KernelRegistry.h"

namespace neverd::emulation {
namespace {

class DriverKernelRegistry : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Object = Scratch;
  static constexpr uint64_t KeyName = Scratch + 0x100;
  static constexpr uint64_t ValueName = Scratch + 0x1000;
  static constexpr uint64_t Data = Scratch + 0x2000;
  static constexpr uint64_t Output = Scratch + 0x3000;
  static constexpr uint64_t HandleOutput = Scratch + 0x4000;
  static constexpr uint64_t LengthOutput = Scratch + 0x4010;
  static constexpr uint64_t Disposition = Scratch + 0x4020;
  static constexpr const char *Path = "\\Registry\\Machine\\Software\\NeverD";
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  std::unique_ptr<KernelRegistry> Registry;

  void success(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }

  void SetUp() override {
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    success(Model->initialize(Image, DriverOptions{}));
    Registry = std::make_unique<KernelRegistry>(*Memory);
    initialize({{Path, {{"Mode", 4, {1, 2, 3, 4}}}}});
    unicode(ValueName, "Mode");
  }

  void initialize(std::vector<DriverRegistryKey> Keys) {
    success(Registry->initialize(std::move(Keys)));
  }

  void put(uint64_t Address, uint64_t Value, unsigned Size = 8) {
    success(Memory->writeInteger(Address, Value, Size));
  }

  uint64_t get(uint64_t Address, unsigned Size = 8) {
    auto Value = Memory->readInteger(Address, Size);
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }

  void unicode(uint64_t Address, llvm::StringRef Text) {
    std::vector<uint8_t> Buffer(Text.size() * 2);
    for (size_t I = 0; I < Text.size(); ++I)
      Buffer[I * 2] = Text[I];
    success(Memory->write(Address + 0x20, Buffer));
    put(Address, Buffer.size(), 2);
    put(Address + 2, Buffer.size(), 2);
    put(Address + 8, Address + 0x20);
  }

  void object(llvm::StringRef Name, uint64_t Root = 0) {
    unicode(KeyName, Name);
    put(Object, 48, 4);
    put(Object + 8, Root);
    put(Object + 16, KeyName);
    put(Object + 24, 0x240, 4);
    put(Object + 32, 0);
    put(Object + 40, 0);
  }

  uint64_t call(llvm::StringRef Name,
                std::initializer_list<uint64_t> Arguments) {
    auto Value = Registry->call(*Model, Name, Arguments);
    if (!Value) {
      ADD_FAILURE() << Name.str() << ": " << llvm::toString(Value.takeError());
      return UINT64_MAX;
    }
    return *Value;
  }

  std::string failure(llvm::StringRef Name,
                      std::initializer_list<uint64_t> Arguments) {
    auto Value = Registry->call(*Model, Name, Arguments);
    if (Value) {
      ADD_FAILURE() << Name.str() << " unexpectedly returned " << *Value;
      return {};
    }
    return llvm::toString(Value.takeError());
  }

  uint64_t open(llvm::StringRef Name = Path, uint32_t Access = 0xf003f,
                uint64_t Root = 0) {
    object(Name, Root);
    EXPECT_EQ(call("ZwOpenKey", {HandleOutput, Access, Object}), 0u);
    return get(HandleOutput);
  }

  uint64_t query(uint64_t Handle, uint32_t Class, uint32_t Size,
                 uint64_t Buffer = Output) {
    return call("ZwQueryValueKey",
                {Handle, ValueName, Class, Buffer, Size, LengthOutput});
  }

  std::vector<uint8_t> bytes(uint64_t Address, size_t Size) {
    std::vector<uint8_t> Bytes(Size);
    success(Memory->read(Address, Bytes));
    return Bytes;
  }
};

TEST_F(DriverKernelRegistry,
       OmittedInventoryStopsButExplicitAbsenceIsConcrete) {
  success(Registry->initialize(std::nullopt));
  EXPECT_FALSE(Registry->snapshot());
  EXPECT_NE(failure("ZwClose", {123}).find("unspecified"), std::string::npos);
  initialize({});
  ASSERT_TRUE(Registry->snapshot());
  EXPECT_TRUE(Registry->snapshot()->empty());
  object(Path);
  EXPECT_EQ(call("ZwOpenKey", {HandleOutput, 1, Object}), 0xc0000034u);
  EXPECT_EQ(call("ZwClose", {123}), 0xc0000008u);
}

TEST_F(DriverKernelRegistry,
       CountedCaseInsensitiveNamesAndRelativeRootHandles) {
  const uint64_t Root = open("\\REGISTRY\\machine\\SOFTWARE", 0x20019);
  const uint64_t Key = open("neverD", 1, Root);
  EXPECT_NE(Root, Key);
  unicode(ValueName, "mODe");
  EXPECT_EQ(query(Key, 2, 16), 0u);
  EXPECT_EQ(get(Output + 4, 4), 4u);
  EXPECT_EQ(get(Output + 8, 4), 4u);
  EXPECT_EQ(get(Output + 12, 4), 0x04030201u);
  EXPECT_EQ(call("ZwClose", {Key}), 0u);
  EXPECT_EQ(call("ZwClose", {Root}), 0u);
  EXPECT_FALSE(Registry->hasOpenHandles());
  EXPECT_EQ(query(Key, 2, 16), 0xc0000008u);
  EXPECT_EQ(call("ZwClose", {Root}), 0xc0000008u);
}

TEST_F(DriverKernelRegistry, PartialQueryDistinguishesShortBufferContracts) {
  const uint64_t Handle = open();
  success(Memory->write(Output, std::vector<uint8_t>(24, 0xcc)));
  EXPECT_EQ(query(Handle, 2, 0, 0), 0xc0000023u);
  EXPECT_EQ(get(LengthOutput, 4), 16u);
  EXPECT_EQ(query(Handle, 2, 11), 0xc0000023u);
  EXPECT_EQ(bytes(Output, 24), std::vector<uint8_t>(24, 0xcc));
  EXPECT_EQ(query(Handle, 2, 13), 0x80000005u);
  EXPECT_EQ(get(LengthOutput, 4), 16u);
  EXPECT_EQ(get(Output, 4), 0u);
  EXPECT_EQ(get(Output + 12, 1), 1u);
  EXPECT_EQ(get(Output + 13, 1), 0xccu);
  EXPECT_EQ(query(Handle, 2, 24), 0u);
  EXPECT_EQ(get(LengthOutput, 4), 16u);
  EXPECT_EQ(bytes(Output + 12, 4), (std::vector<uint8_t>{1, 2, 3, 4}));
  EXPECT_EQ(get(Output + 16, 1), 0xccu);
}

TEST_F(DriverKernelRegistry, BasicAndFullQuerySerializeNamesAndDataOffsets) {
  const uint64_t Handle = open();
  EXPECT_EQ(query(Handle, 0, 20), 0u);
  EXPECT_EQ(get(LengthOutput, 4), 20u);
  EXPECT_EQ(get(Output + 8, 4), 8u);
  EXPECT_EQ(bytes(Output + 12, 8),
            (std::vector<uint8_t>{'M', 0, 'o', 0, 'd', 0, 'e', 0}));
  EXPECT_EQ(query(Handle, 1, 20), 0x80000005u);
  EXPECT_EQ(get(LengthOutput, 4), 32u);
  EXPECT_EQ(get(Output + 8, 4), 28u);
  EXPECT_EQ(get(Output + 12, 4), 4u);
  EXPECT_EQ(get(Output + 16, 4), 8u);
  EXPECT_EQ(query(Handle, 1, 32), 0u);
  EXPECT_EQ(get(Output + 28, 4), 0x04030201u);
}

TEST_F(DriverKernelRegistry,
       Align64QueryUsesDifferentPartialHeaderAndAlignment) {
  const uint64_t Handle = open();
  EXPECT_EQ(query(Handle, 3, 64, Output + 4), 0x80000002u);
  EXPECT_EQ(query(Handle, 4, 64, Output + 4), 0x80000002u);
  EXPECT_EQ(query(Handle, 3, 36), 0u);
  EXPECT_EQ(get(LengthOutput, 4), 36u);
  EXPECT_EQ(get(Output + 8, 4), 32u);
  EXPECT_EQ(get(Output + 32, 4), 0x04030201u);
  EXPECT_EQ(query(Handle, 4, 7), 0xc0000023u);
  EXPECT_EQ(get(LengthOutput, 4), 12u);
  EXPECT_EQ(query(Handle, 4, 8), 0x80000005u);
  EXPECT_EQ(query(Handle, 4, 12), 0u);
  EXPECT_EQ(get(Output, 4), 4u);
  EXPECT_EQ(get(Output + 4, 4), 4u);
  EXPECT_EQ(get(Output + 8, 4), 0x04030201u);
  EXPECT_EQ(query(Handle, 6, 64), 0xc000000du);
  EXPECT_NE(failure("ZwQueryValueKey",
                    {Handle, ValueName, 5, Output, 64, LengthOutput})
                .find("layer"),
            std::string::npos);
}

TEST_F(DriverKernelRegistry,
       HandlesEnforceQuerySetCreateAndDeleteCapabilities) {
  const uint64_t ReadOnly = open(Path, 0x20019);
  EXPECT_EQ(call("ZwSetValueKey", {ReadOnly, ValueName, 0, 4, Data, 4}),
            0xc0000022u);
  EXPECT_EQ(call("ZwDeleteValueKey", {ReadOnly, ValueName}), 0xc0000022u);
  EXPECT_EQ(call("ZwDeleteKey", {ReadOnly}), 0xc0000022u);
  object("Child", ReadOnly);
  EXPECT_EQ(
      call("ZwCreateKey", {HandleOutput, 3, Object, 0, 0, 0, Disposition}),
      0xc0000022u);
  const uint64_t WriteOnly = open(Path, 0x20006);
  EXPECT_EQ(query(WriteOnly, 2, 16), 0xc0000022u);
  put(Data, 0x12345678, 4);
  EXPECT_EQ(call("ZwSetValueKey", {WriteOnly, ValueName, 0, 4, Data, 4}), 0u);
  EXPECT_EQ(query(ReadOnly, 2, 16), 0u);
  EXPECT_EQ(get(Output + 12, 4), 0x12345678u);
}

TEST_F(DriverKernelRegistry,
       CreateDispositionAndValueReplacementAreObservable) {
  const uint64_t Parent = open();
  object("Child", Parent);
  EXPECT_EQ(call("ZwCreateKey",
                 {HandleOutput, 0xf003f, Object, 0, 0, 0, Disposition}),
            0u);
  EXPECT_EQ(get(Disposition, 4), 1u);
  const uint64_t First = get(HandleOutput);
  EXPECT_EQ(call("ZwCreateKey",
                 {HandleOutput, 0xf003f, Object, 0, 0, 0, Disposition}),
            0u);
  EXPECT_EQ(get(Disposition, 4), 2u);
  EXPECT_NE(get(HandleOutput), First);
  unicode(ValueName, "Raw");
  put(Data, 0xdeadbeef, 4);
  EXPECT_EQ(call("ZwSetValueKey", {First, ValueName, 0, 3, Data, 3}), 0u);
  unicode(ValueName, "rAW");
  EXPECT_EQ(call("ZwSetValueKey", {First, ValueName, 0, 4, Data, 2}), 0u);
  EXPECT_EQ(query(First, 2, 14), 0u);
  EXPECT_EQ(get(Output + 8, 4), 2u);
  EXPECT_EQ(get(Output + 12, 2), 0xbeefu);
  auto Snapshot = Registry->snapshot();
  ASSERT_TRUE(Snapshot);
  const auto &Child = Snapshot->back();
  ASSERT_EQ(Child.Values.size(), 1u);
  EXPECT_EQ(Child.Values.front().Name, "Raw");
  EXPECT_EQ(Child.Values.front().Type, 4u);
  EXPECT_EQ(Child.Values.front().Data, (std::vector<uint8_t>{0xef, 0xbe}));
}

TEST_F(DriverKernelRegistry,
       UnnamedValuesUseNullSetNameAndEmptyCountedQueryName) {
  const uint64_t Handle = open();
  EXPECT_EQ(call("ZwSetValueKey", {Handle, 0, 0, 0, 0, 0}), 0u);
  unicode(ValueName, "");
  EXPECT_EQ(query(Handle, 2, 12), 0u);
  EXPECT_EQ(get(LengthOutput, 4), 12u);
  EXPECT_EQ(get(Output + 8, 4), 0u);
  EXPECT_EQ(call("ZwDeleteValueKey", {Handle, ValueName}), 0u);
  EXPECT_EQ(query(Handle, 2, 12), 0xc0000034u);
  EXPECT_EQ(call("ZwDeleteValueKey", {Handle, ValueName}), 0xc0000034u);
}

TEST_F(DriverKernelRegistry, DeletedKeyRemainsInvalidUntilAllHandlesClose) {
  const uint64_t First = open();
  const uint64_t Second = open();
  EXPECT_EQ(call("ZwDeleteKey", {First}), 0u);
  EXPECT_EQ(query(Second, 2, 16), 0xc000017cu);
  EXPECT_EQ(call("ZwDeleteValueKey", {First, ValueName}), 0xc000017cu);
  object(Path);
  EXPECT_EQ(
      call("ZwCreateKey", {HandleOutput, 3, Object, 0, 0, 0, Disposition}),
      0xc000017cu);
  EXPECT_EQ(Registry->snapshot()->size(), 2u);
  EXPECT_EQ(call("ZwClose", {First}), 0u);
  EXPECT_TRUE(Registry->hasOpenHandles());
  EXPECT_EQ(call("ZwClose", {Second}), 0u);
  EXPECT_FALSE(Registry->hasOpenHandles());
  EXPECT_EQ(
      call("ZwCreateKey", {HandleOutput, 3, Object, 0, 0, 0, Disposition}), 0u);
  EXPECT_EQ(get(Disposition, 4), 1u);
  EXPECT_EQ(query(get(HandleOutput), 2, 16), 0xc0000034u);
}

TEST_F(DriverKernelRegistry, ParentDeletionAndVolatileChildrenHaveConstraints) {
  const uint64_t Parent = open("\\Registry\\Machine\\Software");
  EXPECT_EQ(call("ZwDeleteKey", {Parent}), 0xc0000121u);
  object("Volatile", Parent);
  EXPECT_EQ(call("ZwCreateKey",
                 {HandleOutput, 0xf003f, Object, 0, 0, 1, Disposition}),
            0u);
  const uint64_t Volatile = get(HandleOutput);
  object("Child", Volatile);
  EXPECT_EQ(
      call("ZwCreateKey", {HandleOutput, 3, Object, 0, 0, 0, Disposition}),
      0xc0000181u);
  EXPECT_EQ(
      call("ZwCreateKey", {HandleOutput, 3, Object, 0, 0, 1, Disposition}), 0u);
  const uint64_t Machine = open("\\Registry\\Machine");
  EXPECT_EQ(call("ZwDeleteKey", {Machine}), 0xc0000121u);
}

TEST_F(DriverKernelRegistry, UnsupportedNamesSecurityAndAccessDoNotGuess) {
  object(Path);
  EXPECT_NE(
      failure("ZwOpenKey", {HandleOutput, 0x02000000, Object}).find("access"),
      std::string::npos);
  put(Object + 32, Data);
  EXPECT_NE(failure("ZwOpenKey", {HandleOutput, 1, Object}).find("security"),
            std::string::npos);
  object(Path);
  put(KeyName + 0x20, 0xe9, 2);
  EXPECT_NE(failure("ZwOpenKey", {HandleOutput, 1, Object}).find("ASCII"),
            std::string::npos);
  object(Path);
  EXPECT_NE(failure("ZwCreateKey", {HandleOutput, 1, Object, 0, 0, 4, 0})
                .find("options"),
            std::string::npos);
  EXPECT_FALSE(Registry->hasOpenHandles());
}

TEST_F(DriverKernelRegistry, NarrowArgumentsIgnoreUpperRegisterBits) {
  object(Path);
  constexpr uint64_t High = 0xfedcba9800000000;
  EXPECT_EQ(call("ZwOpenKey", {HandleOutput, High | 3, Object}), 0u);
  const uint64_t Handle = get(HandleOutput);
  EXPECT_EQ(call("ZwSetValueKey",
                 {Handle, ValueName, High, High | 4, Data, High | 1}),
            0u);
  EXPECT_EQ(call("ZwQueryValueKey", {Handle, ValueName, High | 2, Output,
                                     High | 13, LengthOutput}),
            0u);
  EXPECT_EQ(get(LengthOutput, 4), 13u);
}

TEST_F(DriverKernelRegistry, FaultingCreateOutputDoesNotPublishKeyOrHandle) {
  object("\\Registry\\Machine\\Software\\NewKey");
  const auto Before = Registry->snapshot()->size();
  EXPECT_NE(
      failure("ZwCreateKey", {0x90000000, 3, Object, 0, 0, 0, Disposition})
          .find("fault"),
      std::string::npos);
  EXPECT_FALSE(Registry->hasOpenHandles());
  EXPECT_EQ(Registry->snapshot()->size(), Before);
}

TEST_F(DriverKernelRegistry, RegistryGuestReadsRespectExpiredModelObjects) {
  put(Object, 48, 4);
  put(Object + 16, Model->registryPath());
  put(Object + 24, 0x240, 4);
  success(Model->finishEntry());
  EXPECT_NE(failure("ZwOpenKey", {HandleOutput, 1, Object}).find("freed"),
            std::string::npos);
  EXPECT_FALSE(Registry->hasOpenHandles());
}

TEST_F(DriverKernelRegistry, BoundsApplyToGuestMutationsAndHandleLifetime) {
  const uint64_t Handle = open();
  EXPECT_NE(failure("ZwSetValueKey",
                    {Handle, ValueName, 0, 3, Data, MaxRegistryValueBytes + 1})
                .find("limit"),
            std::string::npos);
  EXPECT_EQ(query(Handle, 2, 16), 0u);
  EXPECT_EQ(get(Output + 12, 4), 0x04030201u);
  for (size_t I = 1; I < MaxRegistryHandles; ++I)
    open();
  object(Path);
  EXPECT_NE(
      failure("ZwOpenKey", {HandleOutput, 1, Object}).find("handle count"),
      std::string::npos);
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
  EXPECT_EQ(call("ZwOpenKey", {HandleOutput, 1, Object}), 0u);
  EXPECT_NE(get(HandleOutput), Handle);
}

TEST_F(DriverKernelRegistry,
       TotalValueBudgetDoesNotChangeExistingStateOnFailure) {
  DriverRegistryKey Key{Path, {}};
  for (unsigned I = 0; I < 8; ++I)
    Key.Values.push_back(
        {std::to_string(I), 3, std::vector<uint8_t>(65536, I)});
  initialize({std::move(Key)});
  const uint64_t Handle = open();
  unicode(ValueName, "New");
  EXPECT_NE(failure("ZwSetValueKey", {Handle, ValueName, 0, 3, Data, 1})
                .find("total"),
            std::string::npos);
  EXPECT_EQ(query(Handle, 2, 64), 0xc0000034u);
  unicode(ValueName, "0");
  EXPECT_EQ(call("ZwDeleteValueKey", {Handle, ValueName}), 0u);
  unicode(ValueName, "New");
  EXPECT_EQ(call("ZwSetValueKey", {Handle, ValueName, 0, 3, Data, 1}), 0u);
}

TEST(DriverRegistryValidation, RejectsAmbiguousNamesAndUnboundedInventories) {
  auto Invalid = [](std::vector<DriverRegistryKey> Keys) {
    auto E = validateDriverRegistry(Keys);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  };
  Invalid({{"\\Registry\\Machine\\X", {}}, {"\\registry\\machine\\x", {}}});
  Invalid({{"\\Registry\\Machine", {{"A", 3, {}}, {"a", 3, {}}}}});
  Invalid({{"\\Registry\\Machine\\\\X", {}}});
  Invalid({{"\\Registry\\Unknown", {}}});
  Invalid({{"\\Registry\\Machine\\\xc3\xa9", {}}});
  Invalid({{"\\Registry\\Machine", {{"Link", 6, {}}}}});
  Invalid({{"\\Registry\\Machine",
            {{"Large", 3, std::vector<uint8_t>(MaxRegistryValueBytes + 1)}}}});
  std::vector<DriverRegistryKey> Keys;
  for (size_t I = 0; I < MaxRegistryKeys; ++I)
    Keys.push_back({"\\Registry\\Machine\\" + std::to_string(I), {}});
  Invalid(std::move(Keys));
}

} // namespace
} // namespace neverd::emulation
