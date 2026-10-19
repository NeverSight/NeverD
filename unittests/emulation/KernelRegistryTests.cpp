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

#include "RegistryEnumerationTestFixture.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelRegistry.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>

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
    Model->enterExecution(profile::StackBase);
    Registry = std::make_unique<KernelRegistry>(*Model, *Memory);
    initialize({{Path, {{"Mode", 4, {1, 2, 3, 4}}}}});
    unicode(ValueName, "Mode");
  }

  void initialize(std::vector<DriverRegistryKey> Keys,
                  std::optional<uint64_t> Time = std::nullopt,
                  std::optional<std::vector<DriverRegistryHive>> Hives = {}) {
    success(Registry->initialize(std::move(Keys), Time, Hives));
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
    auto Value = Registry->call(Name, Arguments);
    if (!Value) {
      ADD_FAILURE() << Name.str() << ": " << llvm::toString(Value.takeError());
      return UINT64_MAX;
    }
    return *Value;
  }

  std::string failure(llvm::StringRef Name,
                      std::initializer_list<uint64_t> Arguments) {
    auto Value = Registry->call(Name, Arguments);
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

  uint64_t objectCall(llvm::StringRef Name, std::initializer_list<uint64_t> A) {
    auto V = Model->call(Name.str(), A);
    if (!V) {
      ADD_FAILURE() << Name.str() << ": " << llvm::toString(V.takeError());
      return UINT64_MAX;
    }
    return *V;
  }
  std::string objectFailure(llvm::StringRef Name,
                            std::initializer_list<uint64_t> A) {
    auto V = Model->call(Name.str(), A);
    if (V) {
      ADD_FAILURE() << Name.str() << " unexpectedly returned " << *V;
      return {};
    }
    return llvm::toString(V.takeError());
  }
  uint64_t reference(uint64_t Handle, uint64_t Type = 0) {
    EXPECT_EQ(
        objectCall("ObReferenceObjectByHandle", {Handle, 0, Type, 0, Data, 0}),
        0u);
    return get(Data);
  }

  std::vector<uint8_t> bytes(uint64_t Address, size_t Size) {
    std::vector<uint8_t> Bytes(Size);
    success(Memory->read(Address, Bytes));
    return Bytes;
  }
};

TEST_F(DriverKernelRegistry, HiveFlushMatchesNativeKernelModeHandleStatuses) {
  struct Case {
    bool Valid, Deleted;
    uint32_t Status;
  };
  const Case Cases[] = {
#define NEVERD_REGISTRY_FLUSH_NATIVE_CASE(V, D, S) {V, D, S},
#include "RegistryFlushNativeCases.def"
#undef NEVERD_REGISTRY_FLUSH_NATIVE_CASE
  };
  ASSERT_EQ(std::size(Cases), 4u);
  for (const auto &C : Cases) {
    initialize(
        {{Path, {}}}, std::nullopt,
        std::vector<DriverRegistryHive>{{"\\Registry\\Machine\\Software"}});
    const auto Reader = open(Path, 0), Writer = open();
    if (C.Deleted)
      EXPECT_EQ(call("ZwDeleteKey", {Writer}), 0u);
    EXPECT_EQ(call("ZwFlushKey", {C.Valid ? Reader : 0x123}), C.Status);
    EXPECT_EQ(Registry->hiveSnapshot()->at(0).FlushCount, C.Status ? 0u : 1u);
    call("ZwClose", {Reader});
    call("ZwClose", {Writer});
  }
}
TEST_F(DriverKernelRegistry, HiveFlushCommitsSiblingsAndPreservesOtherHives) {
  const std::string Root = "\\Registry\\Machine\\Software";
  const std::string Other = "\\Registry\\Machine\\System\\Other";
  initialize(
      {{Root + "\\A", {{"Mode", 4, {1, 0, 0, 0}}}},
       {Root + "\\B", {{"Mode", 4, {2, 0, 0, 0}}}},
       {Other, {{"Mode", 4, {3, 0, 0, 0}}}}},
      std::nullopt,
      std::vector<DriverRegistryHive>{{Root}, {"\\Registry\\Machine\\System"}});
  const auto A = open(Root + "\\A", 0), B = open(Root + "\\B"), C = open(Other);
  put(Data, 9, 4);
  EXPECT_EQ(call("ZwSetValueKey", {B, ValueName, 0, 4, Data, 4}), 0u);
  put(Data, 8, 4);
  EXPECT_EQ(call("ZwSetValueKey", {C, ValueName, 0, 4, Data, 4}), 0u);
  const auto ReadCommitted = [&](unsigned Hive, llvm::StringRef Path) {
    const auto &H = Registry->hiveSnapshot();
    EXPECT_TRUE(H);
    for (const auto &K : H->at(Hive).Committed)
      if (K.Path == Path)
        return K.Values.at(0).Data.at(0);
    ADD_FAILURE() << Path.str();
    return uint8_t(255);
  };
  EXPECT_EQ(ReadCommitted(0, Root + "\\B"), 2u);
  EXPECT_EQ(call("ZwFlushKey", {A}), 0u);
  EXPECT_EQ(ReadCommitted(0, Root + "\\B"), 9u);
  EXPECT_EQ(ReadCommitted(1, Other), 3u);
  EXPECT_EQ(Registry->hiveSnapshot()->at(0).FlushCount, 1u);
  EXPECT_EQ(Registry->hiveSnapshot()->at(1).FlushCount, 0u);
  put(Data, 7, 4);
  EXPECT_EQ(call("ZwSetValueKey", {B, ValueName, 0, 4, Data, 4}), 0u);
  call("ZwClose", {B});
  EXPECT_EQ(ReadCommitted(0, Root + "\\B"), 9u);
  call("ZwFlushKey", {A});
  EXPECT_EQ(ReadCommitted(0, Root + "\\B"), 7u);
  call("ZwClose", {A});
  call("ZwClose", {C});
}
TEST_F(DriverKernelRegistry, HiveFlushExcludesVolatileKeysAndCommitsDeletion) {
  const std::string Root = "\\Registry\\Machine\\Software";
  initialize({{Path, {{"Mode", 4, {1, 0, 0, 0}}}}}, std::nullopt,
             std::vector<DriverRegistryHive>{{Root}});
  const auto Parent = open();
  object("Transient", Parent);
  EXPECT_EQ(call("ZwCreateKey",
                 {HandleOutput, 0xf003f, Object, 0, 0, 1, Disposition}),
            0u);
  const auto Volatile = get(HandleOutput);
  put(Data, 5, 4);
  EXPECT_EQ(call("ZwSetValueKey", {Volatile, ValueName, 0, 4, Data, 4}), 0u);
  call("ZwFlushKey", {Volatile});
  const auto &Committed = Registry->hiveSnapshot()->at(0).Committed;
  EXPECT_TRUE(
      std::none_of(Committed.begin(), Committed.end(), [](const auto &K) {
        return K.Path.find("Transient") != std::string::npos;
      }));
  call("ZwClose", {Volatile});
  object("Durable", Parent);
  EXPECT_EQ(call("ZwCreateKey",
                 {HandleOutput, 0xf003f, Object, 0, 0, 0, Disposition}),
            0u);
  const auto Durable = get(HandleOutput);
  call("ZwFlushKey", {Parent});
  auto HasDurable = [&] {
    const auto &Keys = Registry->hiveSnapshot()->at(0).Committed;
    return std::any_of(Keys.begin(), Keys.end(), [](const auto &K) {
      return K.Path.find("Durable") != std::string::npos;
    });
  };
  EXPECT_TRUE(HasDurable());
  call("ZwDeleteKey", {Durable});
  EXPECT_EQ(call("ZwFlushKey", {Durable}), 0xc000017cu);
  EXPECT_TRUE(HasDurable());
  call("ZwFlushKey", {Parent});
  EXPECT_FALSE(HasDurable());
  call("ZwClose", {Durable});
  EXPECT_EQ(call("ZwFlushKey", {Durable}), 0xc0000008u);
  call("ZwClose", {Parent});
}
TEST_F(DriverKernelRegistry, HiveFlushRequiresStorageFactsAndPassiveIrql) {
  auto H = open();
  EXPECT_NE(failure("ZwFlushKey", {H}).find("registry_hives"),
            std::string::npos);
  call("ZwClose", {H});
  initialize({{Path, {}}}, std::nullopt, std::vector<DriverRegistryHive>{});
  H = open();
  EXPECT_NE(failure("ZwFlushKey", {H}).find("no declared hive"),
            std::string::npos);
  Model->enterForeground();
  Model->enterExecution(profile::StackBase);
  auto Raised = Model->call("KfRaiseIrql", {1});
  ASSERT_TRUE(bool(Raised));
  auto Flush = Model->call("ZwFlushKey", {H});
  ASSERT_FALSE(bool(Flush));
  EXPECT_NE(llvm::toString(Flush.takeError()).find("PASSIVE_LEVEL"),
            std::string::npos);
  auto Lowered = Model->call("KeLowerIrql", {0});
  ASSERT_TRUE(bool(Lowered));
  call("ZwClose", {H});
}
TEST_F(DriverKernelRegistry,
       HiveMountCannotBeDeletedAndSnapshotsRestoreCommittedData) {
  const std::string Root = "\\Registry\\Machine\\Software";
  initialize({{Path, {{"Mode", 4, {1, 0, 0, 0}}}}}, std::nullopt,
             std::vector<DriverRegistryHive>{{Root}});
  const auto H = open(), Mount = open(Root);
  EXPECT_EQ(call("ZwDeleteKey", {Mount}), 0xc0000121u);
  put(Data, 42, 4);
  EXPECT_EQ(call("ZwSetValueKey", {H, ValueName, 0, 4, Data, 4}), 0u);
  call("ZwFlushKey", {H});
  const auto Committed = Registry->hiveSnapshot()->at(0).Committed;
  put(Data, 43, 4);
  call("ZwSetValueKey", {H, ValueName, 0, 4, Data, 4});
  call("ZwClose", {H});
  call("ZwClose", {Mount});
  initialize(Committed, std::nullopt, std::vector<DriverRegistryHive>{{Root}});
  const auto Restored = open();
  EXPECT_EQ(query(Restored, 2, 16), 0u);
  EXPECT_EQ(get(Output + 12, 4), 42u);
  call("ZwClose", {Restored});
}
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
  EXPECT_EQ(get(LengthOutput, 4), 36u);
  EXPECT_EQ(get(Output + 8, 4), 32u);
  EXPECT_EQ(get(Output + 12, 4), 4u);
  EXPECT_EQ(get(Output + 16, 4), 8u);
  EXPECT_EQ(query(Handle, 1, 36), 0u);
  EXPECT_EQ(get(Output + 32, 4), 0x04030201u);
}

TEST_F(DriverKernelRegistry, EnumerationUsesCreationOrderAndLiveValueIndices) {
  initialize(
      {{Path, {{"Zulu", 3, {0x11}}, {"Alpha", 4, {0x22}}, {"", 0, {}}}}});
  const auto Handle = open();
  auto Enumerate = [&](uint64_t Index, uint64_t Class = 0) {
    return call("ZwEnumerateValueKey",
                {Handle, Index, Class, Output, 64, LengthOutput});
  };
  EXPECT_EQ(Enumerate(0), 0u);
  EXPECT_EQ(get(Output + 12, 2), 'Z');
  EXPECT_EQ(Enumerate(1), 0u);
  EXPECT_EQ(get(Output + 12, 2), 'A');
  EXPECT_EQ(Enumerate(2), 0u);
  EXPECT_EQ(get(Output + 8, 4), 0u);
  EXPECT_EQ(Enumerate(3), 0x8000001au);
  // Replacing a value preserves its original name and position.
  unicode(ValueName, "zULU");
  put(Data, 0x33, 1);
  EXPECT_EQ(call("ZwSetValueKey", {Handle, ValueName, 0, 3, Data, 1}), 0u);
  EXPECT_EQ(Enumerate(0, 1), 0u);
  EXPECT_EQ(get(Output + 20, 2), 'Z');
  EXPECT_EQ(get(Output + 32, 1), 0x33u);
  // Removing compacts the list; recreating appends, even for the same spelling.
  EXPECT_EQ(call("ZwDeleteValueKey", {Handle, ValueName}), 0u);
  EXPECT_EQ(Enumerate(0), 0u);
  EXPECT_EQ(get(Output + 12, 2), 'A');
  EXPECT_EQ(call("ZwSetValueKey", {Handle, ValueName, 0, 3, Data, 1}), 0u);
  EXPECT_EQ(Enumerate(2), 0u);
  EXPECT_EQ(get(Output + 12, 2), 'z');
  const auto Snapshot = Registry->snapshot();
  ASSERT_TRUE(Snapshot);
  const auto &Values = Snapshot->back().Values;
  ASSERT_EQ(Values.size(), 3u);
  EXPECT_EQ(Values[0].Name, "Alpha");
  EXPECT_EQ(Values[1].Name, "");
  EXPECT_EQ(Values[2].Name, "zULU");
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
  initialize(*Snapshot);
  const auto Reopened = open();
  EXPECT_EQ(
      call("ZwEnumerateValueKey", {Reopened, 2, 0, Output, 64, LengthOutput}),
      0u);
  EXPECT_EQ(get(Output + 12, 2), 'z');
}

TEST_F(DriverKernelRegistry, EnumerationSharesQueryLayoutsAndShortBuffers) {
  const auto Handle = open();
  for (uint32_t Class : {0, 1, 2}) {
    for (uint32_t Length = 0; Length <= 40; ++Length) {
      SCOPED_TRACE(::testing::Message() << Class << ':' << Length);
      success(Memory->write(Output, std::vector<uint8_t>(64, 0xcc)));
      const auto Status = query(Handle, Class, Length);
      const auto Required = get(LengthOutput, 4);
      const auto Expected = bytes(Output, 64);
      success(Memory->write(Output, std::vector<uint8_t>(64, 0xcc)));
      EXPECT_EQ(call("ZwEnumerateValueKey",
                     {Handle, 0, Class, Output, Length, LengthOutput}),
                Status);
      EXPECT_EQ(get(LengthOutput, 4), Required);
      EXPECT_EQ(bytes(Output, 64), Expected);
    }
  }
  // Independently checked against CmpQueryKeyValueData in 19041.6456:
  // Full data is eight-aligned; padding and an incomplete ASCII unit survive.
  success(Memory->write(Output, std::vector<uint8_t>(64, 0xcc)));
  EXPECT_EQ(query(Handle, 1, 64), 0u);
  EXPECT_EQ(get(Output + 8, 4), 32u);
  EXPECT_EQ(get(Output + 28, 4), 0xccccccccu);
  success(Memory->write(Output, std::vector<uint8_t>(64, 0xcc)));
  EXPECT_EQ(query(Handle, 0, 13), 0x80000005u);
  EXPECT_EQ(get(Output + 12, 2), 0xccccu);
  EXPECT_EQ(call("ZwSetValueKey", {Handle, ValueName, 0, 0, 0, 0}), 0u);
  EXPECT_EQ(query(Handle, 1, 28), 0u);
  EXPECT_EQ(get(LengthOutput, 4), 28u);
  EXPECT_EQ(get(Output + 8, 4), UINT32_MAX);
  EXPECT_EQ(get(Output + 12, 4), 0u);
  EXPECT_EQ(call("ZwEnumerateValueKey", {Handle, 0, 0, Output, 64, Output}),
            0u);
  EXPECT_EQ(get(Output, 4), 20u);
  EXPECT_EQ(get(Output + 8, 4), 8u);
}

TEST_F(DriverKernelRegistry, EnumerationErrorsPreserveOutputsAndRespectRights) {
  const auto Handle = open();
  put(Output, 0x1122334455667788);
  put(LengthOutput, 0x98765432, 4);
  // Invalid enum classes precede handle, index and output access in the kernel.
  for (uint64_t Class : {3, 4, 5, 6, -1})
    EXPECT_EQ(call("ZwEnumerateValueKey", {0, 0, Class, 0, 0, 0}), 0xc000000du);
  EXPECT_EQ(call("ZwEnumerateValueKey", {0, 0, 0, 0, 0, 0}), 0xc0000008u);
  EXPECT_EQ(call("ZwEnumerateValueKey", {Handle, UINT32_MAX, 0, 0, 0, 0}),
            0x8000001au);
  EXPECT_EQ(call("ZwEnumerateValueKey", {open(Path, 2), 0, 0, 0, 0, 0}),
            0xc0000022u);
  EXPECT_FALSE(failure("ZwEnumerateValueKey",
                       {Handle, 0, 0, 0x90000000, 64, LengthOutput})
                   .empty());
  EXPECT_FALSE(
      failure("ZwEnumerateValueKey", {Handle, 0, 0, Output, 64, 0x90000000})
          .empty());
  EXPECT_EQ(get(Output), 0x1122334455667788u);
  EXPECT_EQ(get(LengthOutput, 4), 0x98765432u);
  constexpr uint64_t High = 0xfedcba9800000000;
  EXPECT_EQ(call("ZwEnumerateValueKey",
                 {Handle, High, High | 2, Output, High | 16, LengthOutput}),
            0u);
  EXPECT_EQ(get(Output + 12, 4), 0x04030201u);
  EXPECT_EQ(call("ZwDeleteKey", {Handle}), 0u);
  EXPECT_EQ(call("ZwEnumerateValueKey", {Handle, 0, 0, 0, 0, 0}), 0xc000017cu);
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
  EXPECT_EQ(call("ZwEnumerateValueKey", {Handle, 0, 0, 0, 0, 0}), 0xc0000008u);
}

TEST_F(DriverKernelRegistry, KeyInformationSerializesNamesCountsAndTimestamps) {
  initialize({{Path, {{"Mode", 4, {1, 2, 3, 4}}, {"Longer", 3, {1}}}, 1234},
              {std::string(Path) + "\\z", {}},
              {std::string(Path) + "\\Alpha\\Grandchild", {}}},
             2000);
  const auto Handle = open();
  auto Query = [&](uint32_t Class, uint32_t Size) {
    return call("ZwQueryKey", {Handle, Class, Output, Size, LengthOutput});
  };
  success(Memory->write(Output, std::vector<uint8_t>(64, 0xcc)));
  EXPECT_EQ(Query(0, 15), 0xc0000023u);
  EXPECT_EQ(get(LengthOutput, 4), 28u);
  EXPECT_EQ(get(Output), 0xccccccccccccccccu);
  EXPECT_EQ(Query(0, 17), 0x80000005u);
  EXPECT_EQ(get(Output), 1234u);
  EXPECT_EQ(get(Output + 8, 4), 0u);
  EXPECT_EQ(get(Output + 12, 4), 12u);
  EXPECT_EQ(get(Output + 16, 2), 0xccccu);
  EXPECT_EQ(Query(0, 28), 0u);
  EXPECT_EQ(get(Output + 16, 2), 'N');
  EXPECT_EQ(Query(1, 64), 0u);
  EXPECT_EQ(get(LengthOutput, 4), 36u);
  EXPECT_EQ(get(Output + 12, 4), UINT32_MAX);
  EXPECT_EQ(get(Output + 16, 4), 0u);
  EXPECT_EQ(get(Output + 20, 4), 12u);
  EXPECT_EQ(get(Output + 24, 2), 'N');
  EXPECT_EQ(Query(2, 44), 0u);
  EXPECT_EQ(get(LengthOutput, 4), 44u);
  EXPECT_EQ(get(Output), 1234u);
  EXPECT_EQ(get(Output + 12, 4), UINT32_MAX);
  EXPECT_EQ(get(Output + 16, 4), 0u);
  EXPECT_EQ(get(Output + 20, 4), 2u);
  EXPECT_EQ(get(Output + 24, 4), 10u);
  EXPECT_EQ(get(Output + 28, 4), 0u);
  EXPECT_EQ(get(Output + 32, 4), 2u);
  EXPECT_EQ(get(Output + 36, 4), 12u);
  EXPECT_EQ(get(Output + 40, 4), 4u);
  success(Memory->write(Output, std::vector<uint8_t>(48, 0xcc)));
  EXPECT_EQ(Query(4, 39), 0xc0000023u);
  EXPECT_EQ(get(Output), 0xccccccccccccccccu);
  EXPECT_EQ(Query(4, 40), 0u);
  EXPECT_EQ(get(LengthOutput, 4), 40u);
  EXPECT_EQ(get(Output + 12, 4), 2u);
  EXPECT_EQ(get(Output + 16, 4), 10u);
  EXPECT_EQ(get(Output + 20, 4), 2u);
  EXPECT_EQ(get(Output + 24, 4), 12u);
  EXPECT_EQ(get(Output + 28, 4), 4u);
  EXPECT_EQ(get(Output + 32, 4), 12u);
  EXPECT_EQ(get(Output + 36, 4), 0xccccccccu);
}

TEST_F(DriverKernelRegistry, KeyNamesNeedNoClockAndUseAnyNonzeroGrantedAccess) {
  const auto Handle = open(Path, 2);
  const uint32_t Required = 4 + std::string(Path).size() * 2;
  EXPECT_EQ(call("ZwQueryKey", {Handle, 3, 0, 0, LengthOutput}), 0xc0000023u);
  EXPECT_EQ(get(LengthOutput, 4), Required);
  success(Memory->write(Output, std::vector<uint8_t>(128, 0xcc)));
  EXPECT_EQ(call("ZwQueryKey", {Handle, 3, Output, 5, LengthOutput}),
            0x80000005u);
  EXPECT_EQ(get(Output, 4), Required - 4);
  EXPECT_EQ(get(Output + 4, 2), 0xcc5cu); // NameInformation copies odd bytes.
  EXPECT_EQ(call("ZwQueryKey", {Handle, 3, Output, 128, LengthOutput}), 0u);
  EXPECT_EQ(get(Output + 4, 2), '\\');
  EXPECT_EQ(call("ZwQueryKey", {Handle, 0, Output, 64, LengthOutput}),
            0xc0000022u);
  EXPECT_EQ(call("ZwQueryKey", {open(Path, 0), 3, 0, 0, 0}), 0xc0000022u);
  EXPECT_EQ(call("ZwQueryKey", {0, 9, 0, 0, 0}), 0xc000000du);
  const auto Read = open();
  EXPECT_EQ(call("ZwQueryKey", {Read, 0, 0, 0, LengthOutput}), 0xc0000023u);
  EXPECT_NE(failure("ZwQueryKey", {Read, 0, Output, 64, LengthOutput})
                .find("last-write time is unspecified"),
            std::string::npos);
  EXPECT_NE(failure("ZwQueryKey", {Read, 5, Output, 64, LengthOutput})
                .find("unsupported"),
            std::string::npos);
  constexpr uint64_t High = 0xaabbccdd00000000;
  EXPECT_EQ(
      call("ZwQueryKey", {Read, High | 3, Output, High | 128, LengthOutput}),
      0u);
}

TEST_F(DriverKernelRegistry, SubkeyEnumerationUsesImmediateLiveChildren) {
  initialize({{Path, {}},
              {std::string(Path) + "\\_item", {}},
              {std::string(Path) + "\\z", {}},
              {std::string(Path) + "\\Alpha\\Grandchild", {}}},
             99);
  const auto Handle = open(Path, 8);
  const char Initials[] = {'A', 'z', '_'};
  for (unsigned I = 0; I < 3; ++I) {
    EXPECT_EQ(call("ZwEnumerateKey", {Handle, I, 0, Output, 64, LengthOutput}),
              0u);
    EXPECT_EQ(get(Output), 99u);
    EXPECT_EQ(get(Output + 16, 2), Initials[I]);
  }
  EXPECT_EQ(call("ZwEnumerateKey", {Handle, 3, 0, 0, 0, 0}), 0x8000001au);
  EXPECT_EQ(call("ZwEnumerateKey", {Handle, UINT32_MAX, 0, 0, 0, 0}),
            0x8000001au);
  EXPECT_EQ(call("ZwEnumerateKey", {0, 0, 3, 0, 0, 0}), 0xc000000du);
  EXPECT_EQ(call("ZwEnumerateKey", {open(Path, 1), 0, 0, 0, 0, 0}),
            0xc0000022u);
  // Enumerating Full describes the child, including its own immediate children.
  EXPECT_EQ(call("ZwEnumerateKey", {Handle, 0, 2, Output, 44, LengthOutput}),
            0u);
  EXPECT_EQ(get(Output + 20, 4), 1u);
  EXPECT_EQ(get(Output + 24, 4), 20u);
  const auto Z = open(std::string(Path) + "\\z");
  EXPECT_EQ(call("ZwDeleteKey", {Z}), 0u);
  EXPECT_EQ(call("ZwEnumerateKey", {Handle, 1, 1, Output, 64, LengthOutput}),
            0u);
  EXPECT_EQ(get(Output + 24, 2), '_');
  EXPECT_EQ(call("ZwEnumerateKey", {Z, 0, 0, 0, 0, 0}), 0xc000017cu);
  constexpr uint64_t High = 0xabcdef0100000000;
  EXPECT_EQ(call("ZwEnumerateKey",
                 {Handle, High | 1, High, Output, High | 64, LengthOutput}),
            0u);
  EXPECT_EQ(get(Output + 16, 2), '_');
}

TEST_F(DriverKernelRegistry, KeyQueryOutputsAndDeletedNamesMatchTheKernel) {
  initialize({{Path, {}, 1234}});
  const auto Handle = open();
  put(Output, 0x1122334455667788);
  put(LengthOutput, 0x98765432, 4);
  EXPECT_FALSE(
      failure("ZwQueryKey", {Handle, 0, 0x90000000, 64, LengthOutput}).empty());
  EXPECT_FALSE(
      failure("ZwQueryKey", {Handle, 0, Output, 64, 0x90000000}).empty());
  EXPECT_EQ(get(Output), 0x1122334455667788u);
  EXPECT_EQ(get(LengthOutput, 4), 0x98765432u);
  EXPECT_EQ(call("ZwQueryKey", {Handle, 0, Output, 64, Output}), 0u);
  EXPECT_EQ(get(Output, 4), 28u); // ResultLength follows the aliased payload.
  EXPECT_EQ(get(Output + 12, 4), 12u);
  EXPECT_EQ(call("ZwDeleteKey", {Handle}), 0u);
  EXPECT_EQ(call("ZwQueryKey", {Handle, 0, 0, 0, 0}), 0xc000017cu);
  EXPECT_EQ(call("ZwQueryKey", {Handle, 3, 0, 0, LengthOutput}), 0xc0000023u);
  EXPECT_EQ(call("ZwQueryKey", {Handle, 3, Output, 5, LengthOutput}),
            0x80000005u);
  EXPECT_EQ(call("ZwQueryKey", {Handle, 3, Output, 128, LengthOutput}),
            0xc000017cu);
  EXPECT_EQ(get(Output, 4), std::string(Path).size() * 2);
  EXPECT_EQ(get(Output + 4, 2), '\\');
}

TEST_F(DriverKernelRegistry, KeyFullNameUsesEachRetainedAncestorSpelling) {
  initialize({{"\\Registry\\Machine\\Software", {}},
              {"\\registry\\machine\\SOFTWARE\\Child", {}}});
  const auto Handle = open("\\REGISTRY\\MACHINE\\software\\CHILD");
  EXPECT_EQ(call("ZwQueryKey", {Handle, 3, Output, 128, LengthOutput}), 0u);
  const std::string Expected = "\\Registry\\Machine\\Software\\Child";
  EXPECT_EQ(get(Output, 4), Expected.size() * 2);
  for (size_t I = 0; I < Expected.size(); ++I)
    EXPECT_EQ(get(Output + 4 + I * 2, 2), uint8_t(Expected[I]));
  EXPECT_EQ(call("ZwDeleteKey", {Handle}), 0u);
  const auto Parent = open("\\Registry\\Machine\\Software");
  EXPECT_EQ(call("ZwDeleteKey", {Parent}), 0u);
  EXPECT_EQ(call("ZwClose", {Parent}), 0u);
  EXPECT_EQ(call("ZwQueryKey", {Handle, 3, Output, 128, LengthOutput}),
            0xc000017cu);
  for (size_t I = 0; I < Expected.size(); ++I)
    EXPECT_EQ(get(Output + 4 + I * 2, 2), uint8_t(Expected[I]));
}

TEST_F(DriverKernelRegistry, RegistryMutationsUseDeclaredClockAndRetainMaxima) {
  initialize({{Path, {{"LongName", 3, {1, 2, 3, 4}}, {"B", 3, {0}}}, 17}},
             1000);
  const auto Handle = open();
  auto Query = [&]() {
    EXPECT_EQ(call("ZwQueryKey", {Handle, 2, Output, 44, LengthOutput}), 0u);
  };
  success(Model->advanceExecutionTo100ns(20));
  unicode(ValueName, "LongName");
  put(Data, 0x04030201, 4);
  EXPECT_EQ(call("ZwSetValueKey", {Handle, ValueName, 0, 3, Data, 4}), 0u);
  Query();
  EXPECT_EQ(get(Output), 17u); // Identical writes preserve the observation.
  EXPECT_EQ(call("ZwSetValueKey", {Handle, ValueName, 0, 3, Data, 1}), 0u);
  Query();
  EXPECT_EQ(get(Output), 1020u);
  EXPECT_EQ(get(Output + 40, 4),
            4u); // Shrinking a value preserves the maximum.
  EXPECT_EQ(call("ZwDeleteValueKey", {Handle, ValueName}), 0u);
  Query();
  EXPECT_EQ(get(Output + 36, 4), 16u);
  EXPECT_EQ(get(Output + 40, 4), 4u);
  unicode(ValueName, "B");
  EXPECT_EQ(call("ZwDeleteValueKey", {Handle, ValueName}), 0u);
  Query();
  EXPECT_EQ(get(Output + 36, 4), 0u);
  EXPECT_EQ(get(Output + 40, 4), 0u);
  success(Model->advanceExecutionTo100ns(30));
  object("LongChild", Handle);
  EXPECT_EQ(call("ZwCreateKey",
                 {HandleOutput, 0xf003f, Object, 0, 0, 0, Disposition}),
            0u);
  const auto Child = get(HandleOutput);
  Query();
  EXPECT_EQ(get(Output), 1030u);
  EXPECT_EQ(get(Output + 24, 4), 18u);
  EXPECT_EQ(call("ZwQueryKey", {Child, 0, Output, 64, LengthOutput}), 0u);
  EXPECT_EQ(get(Output), 1030u);
  success(Model->advanceExecutionTo100ns(40));
  EXPECT_EQ(call("ZwDeleteKey", {Child}), 0u);
  Query();
  EXPECT_EQ(get(Output), 1040u);
  EXPECT_EQ(get(Output + 24, 4), 0u);
  EXPECT_EQ(Registry->snapshot()->back().LastWriteTime100ns, 1040u);
}

TEST_F(DriverKernelRegistry, UnknownAndOverflowingRegistryTimesDoNotGuess) {
  initialize({{Path, {{"Mode", 4, {1, 2, 3, 4}}}, 42}});
  auto Handle = open();
  put(Data, 0x04030201, 4);
  EXPECT_EQ(call("ZwSetValueKey", {Handle, ValueName, 0, 4, Data, 4}), 0u);
  EXPECT_EQ(call("ZwQueryKey", {Handle, 0, Output, 64, LengthOutput}), 0u);
  EXPECT_EQ(get(Output), 42u);
  put(Data, 9, 4);
  EXPECT_EQ(call("ZwSetValueKey", {Handle, ValueName, 0, 4, Data, 4}), 0u);
  EXPECT_FALSE(Registry->snapshot()->back().LastWriteTime100ns);
  EXPECT_NE(failure("ZwQueryKey", {Handle, 0, Output, 64, LengthOutput})
                .find("unspecified"),
            std::string::npos);
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
  initialize({{Path, {{"Mode", 4, {1, 2, 3, 4}}}}}, uint64_t(INT64_MAX));
  Handle = open();
  success(Model->advanceExecutionTo100ns(1));
  EXPECT_NE(failure("ZwSetValueKey", {Handle, ValueName, 0, 4, Data, 4})
                .find("time exceeds"),
            std::string::npos);
  EXPECT_EQ(query(Handle, 2, 16), 0u);
  EXPECT_EQ(get(Output + 12, 4), 0x04030201u);
  EXPECT_NE(
      failure("ZwDeleteValueKey", {Handle, ValueName}).find("time exceeds"),
      std::string::npos);
  object("Child", Handle);
  EXPECT_NE(
      failure("ZwCreateKey", {HandleOutput, 3, Object, 0, 0, 0, Disposition})
          .find("time exceeds"),
      std::string::npos);
  EXPECT_EQ(Registry->snapshot()->size(), 3u);
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

TEST_F(DriverKernelRegistry, DeletedKeyBodiesRemainSeparateFromARecreatedName) {
  const uint64_t First = open();
  const uint64_t Second = open();
  EXPECT_EQ(call("ZwDeleteKey", {First}), 0u);
  EXPECT_EQ(call("ZwDeleteKey", {First}), 0u);
  EXPECT_EQ(query(Second, 2, 16), 0xc000017cu);
  EXPECT_EQ(call("ZwDeleteValueKey", {First, ValueName}), 0xc000017cu);
  EXPECT_EQ(Registry->snapshot()->size(), 2u);
  object(Path);
  EXPECT_EQ(call("ZwOpenKey", {HandleOutput, 1, Object}), 0xc0000034u);
  EXPECT_EQ(
      call("ZwCreateKey", {HandleOutput, 3, Object, 0, 0, 0, Disposition}), 0u);
  const uint64_t Recreated = get(HandleOutput);
  EXPECT_EQ(get(Disposition, 4), 1u);
  EXPECT_EQ(query(Recreated, 2, 16), 0xc0000034u);
  EXPECT_EQ(query(Second, 2, 16), 0xc000017cu);
  EXPECT_EQ(call("ZwClose", {First}), 0u);
  EXPECT_EQ(call("ZwClose", {Second}), 0u);
  EXPECT_TRUE(Registry->hasOpenHandles());
  EXPECT_EQ(query(Recreated, 2, 16), 0xc0000034u);
  EXPECT_EQ(call("ZwClose", {Recreated}), 0u);
  EXPECT_FALSE(Registry->hasOpenHandles());
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

TEST_F(DriverKernelRegistry, GenericReadAndExecuteRetainReadOnlyCapabilities) {
  for (uint32_t Access : {0x80000000u, 0x20000000u}) {
    SCOPED_TRACE(Access);
    const auto H = open(Path, Access);
    EXPECT_EQ(query(H, 2, 16), 0u);
    EXPECT_EQ(call("ZwSetValueKey", {H, ValueName, 0, 4, Data, 4}),
              0xc0000022u);
    EXPECT_EQ(call("ZwDeleteValueKey", {H, ValueName}), 0xc0000022u);
    EXPECT_EQ(call("ZwDeleteKey", {H}), 0xc0000022u);
    object("BlockedChild", H);
    EXPECT_EQ(call("ZwCreateKey",
                   {HandleOutput, 0x10000000, Object, 0, 0, 0, Disposition}),
              0xc0000022u);
    EXPECT_EQ(call("ZwClose", {H}), 0u);
  }
}

TEST_F(DriverKernelRegistry, GenericWriteCanMutateAndCreateButCannotQuery) {
  const auto H = open(Path, 0x40000000);
  put(Data, 0x12345678, 4);
  EXPECT_EQ(call("ZwSetValueKey", {H, ValueName, 0, 4, Data, 4}), 0u);
  EXPECT_EQ(query(H, 2, 16), 0xc0000022u);
  EXPECT_EQ(call("ZwDeleteKey", {H}), 0xc0000022u);
  object("WritableChild", H);
  EXPECT_EQ(call("ZwCreateKey",
                 {HandleOutput, 0x10000000, Object, 0, 0, 0, Disposition}),
            0u);
  const auto Child = get(HandleOutput);
  EXPECT_EQ(call("ZwDeleteKey", {Child}), 0u);
  EXPECT_EQ(call("ZwClose", {Child}), 0u);
  EXPECT_EQ(call("ZwDeleteValueKey", {H, ValueName}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(DriverKernelRegistry,
       GenericMasksUnionWithExplicitRightsAndIgnoreUpperBits) {
  for (uint64_t Access :
       {0xc0000000ULL, 0x80000002ULL, 0xdeadbeef80000002ULL}) {
    object(Path);
    ASSERT_EQ(call("ZwOpenKey", {HandleOutput, Access, Object}), 0u);
    const auto H = get(HandleOutput);
    put(Data, 0x98765432, 4);
    EXPECT_EQ(call("ZwSetValueKey", {H, ValueName, 0, 4, Data, 4}), 0u);
    EXPECT_EQ(query(H, 2, 16), 0u);
    EXPECT_EQ(get(Output + 12, 4), 0x98765432u);
    EXPECT_EQ(call("ZwDeleteKey", {H}), 0xc0000022u);
    EXPECT_EQ(call("ZwClose", {H}), 0u);
  }
  const auto H = open(Path, 0x10000000);
  EXPECT_EQ(query(H, 2, 16), 0u);
  EXPECT_EQ(call("ZwDeleteKey", {H}), 0u);
  EXPECT_EQ(query(H, 2, 16), 0xc000017cu);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(DriverKernelRegistry,
       GenericOpenMissingKeyAndUnsupportedModifiersKeepOutputs) {
  for (uint32_t Access :
       {0x82000000u, 0x80000100u, 0x80000200u, 0x81000000u, 0x80100000u}) {
    object(Path);
    put(HandleOutput, 0x77777777);
    EXPECT_NE(failure("ZwOpenKey", {HandleOutput, Access, Object})
                  .find("access mask"),
              std::string::npos);
    EXPECT_EQ(get(HandleOutput), 0x77777777u);
    EXPECT_FALSE(Registry->hasOpenHandles());
  }
  object("\\Registry\\Machine\\Software\\Missing");
  put(HandleOutput, 0x77777777);
  EXPECT_EQ(call("ZwOpenKey", {HandleOutput, 0x80000000, Object}), 0xc0000034u);
  EXPECT_EQ(get(HandleOutput), 0x77777777u);
  EXPECT_FALSE(Registry->hasOpenHandles());
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
  EXPECT_FALSE(
      failure("ZwCreateKey", {0x90000000, 3, Object, 0, 0, 0, Disposition})
          .empty());
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

TEST_F(DriverKernelRegistry,
       PerOpenObjectsShareKeyDataAndCommonHandleRetirement) {
  const auto First = open(), Second = open();
  const auto A = reference(First), B = reference(Second);
  EXPECT_NE(A, B);
  const auto Type = objectCall("ObGetObjectType", {A});
  EXPECT_EQ(objectCall("ObGetObjectType", {B}), Type);
  unicode(KeyName, Path);
  EXPECT_NE(objectFailure("ObReferenceObjectByName",
                          {KeyName, 0x40, 0, 0, Type, 0, 0, Data})
                .find("registry name parsing"),
            std::string::npos);
  auto Opaque = Model->validateGuestAccess(A, 1, false);
  ASSERT_TRUE(bool(Opaque));
  EXPECT_NE(llvm::toString(std::move(Opaque)).find("opaque"),
            std::string::npos);
  EXPECT_EQ(
      objectCall("ObReferenceObjectByHandle", {First, 0, Type, 0, Data, 0}),
      0u);
  EXPECT_EQ(get(Data), A);
  objectCall("ObfDereferenceObject", {A});
  const auto DriverType =
      objectCall("ObGetObjectType", {Model->driverObject()});
  EXPECT_EQ(objectCall("ObReferenceObjectByHandle",
                       {First, 0, DriverType, 0, Data, 0}),
            0xc0000024u);
  EXPECT_EQ(objectCall("ObReferenceObjectByPointer", {A, 0, DriverType, 0}),
            0xc0000024u);
  EXPECT_EQ(objectCall("ObReferenceObjectByPointer", {A, 0, Type, 0}), 0u);
  objectCall("ObfDereferenceObject", {A});
  EXPECT_EQ(objectCall("ObOpenObjectByPointer",
                       {A, 0x200, 0, 0x80000000, Type, 0, HandleOutput}),
            0u);
  const auto Clone = get(HandleOutput);
  EXPECT_EQ(reference(Clone, Type), A);
  objectCall("ObfDereferenceObject", {A});
  EXPECT_EQ(objectCall("ZwClose", {First}), 0u);
  EXPECT_EQ(query(Clone, 2, 16), 0u);
  EXPECT_EQ(query(Second, 2, 16), 0u);
  EXPECT_EQ(objectCall("ZwClose", {Clone}), 0u);
  EXPECT_EQ(objectCall("ZwClose", {Clone}), 0xc0000008u);
  EXPECT_EQ(objectCall("ObGetObjectType", {A}), Type);
  objectCall("ObfDereferenceObject", {A});
  EXPECT_FALSE(objectFailure("ObGetObjectType", {A}).empty());
  EXPECT_EQ(objectCall("ZwClose", {Second}), 0u);
  objectCall("ObfDereferenceObject", {B});
  EXPECT_FALSE(Registry->hasOpenHandles());
  EXPECT_FALSE(objectFailure("ObfDereferenceObject", {B}).empty());
  auto Invalid = Model->validateGuestAccess(B, 1, false);
  ASSERT_TRUE(bool(Invalid));
  EXPECT_NE(llvm::toString(std::move(Invalid)).find("freed"),
            std::string::npos);
}

TEST_F(DriverKernelRegistry, KeyNameBytesMatchShortOddExactAndDeletedQueries) {
  const auto Handle = open();
  const auto Body = reference(Handle);
  const uint32_t Required = 18 + std::string(Path).size() * 2;
  for (bool ByHandle : {false, true}) {
    for (uint32_t Size = 0; Size <= Required + 2; ++Size) {
      SCOPED_TRACE(::testing::Message() << ByHandle << ':' << Size);
      success(Memory->write(Output, std::vector<uint8_t>(128, 0xa5)));
      const auto Status =
          ByHandle ? objectCall("ZwQueryObject",
                                {Handle, 1, Output, Size, LengthOutput})
                   : objectCall("ObQueryNameString",
                                {Body, Output, Size, LengthOutput});
      EXPECT_EQ(Status, Size < Required ? 0xc0000004u : 0u);
      EXPECT_EQ(get(LengthOutput, 4), Required);
      if (Size < 18) {
        EXPECT_EQ(bytes(Output, 128), std::vector<uint8_t>(128, 0xa5));
        continue;
      }
      const auto Written = std::min(Size, Required) - 18;
      EXPECT_EQ(get(Output, 2), Written);
      EXPECT_EQ(get(Output + 2, 2), Written);
      EXPECT_EQ(get(Output + 4, 4), Size >= Required ? 0u : 0xa5a5a5a5u);
      EXPECT_EQ(get(Output + 8), Output + 16);
      for (unsigned I = 0; I < (Written & ~1u); I += 2)
        EXPECT_EQ(get(Output + 16 + I, 2), uint8_t(Path[I / 2]));
      EXPECT_EQ(get(Output + 16 + (Written & ~1u), 2), 0u);
    }
  }
  EXPECT_EQ(objectCall("ZwQueryObject", {Handle, 1, Output, 128, 0}), 0u);
  EXPECT_EQ(objectCall("ObQueryNameString", {Body, Output, 128, Output}), 0u);
  EXPECT_EQ(get(Output, 2), Required - 18);
  EXPECT_EQ(objectCall("ZwQueryObject", {Handle, 1, Output, 128, Output}), 0u);
  EXPECT_EQ(get(Output, 4),
            Required); // Syscall result length follows the payload.
  EXPECT_EQ(call("ZwDeleteKey", {Handle}), 0u);
  success(Memory->write(Output, std::vector<uint8_t>(128, 0xa5)));
  put(LengthOutput, 0xfeed, 4);
  EXPECT_EQ(objectCall("ObQueryNameString", {Body, Output, 128, LengthOutput}),
            0xc000017cu);
  EXPECT_EQ(get(LengthOutput, 4), 0xfeedu);
  EXPECT_EQ(objectCall("ZwQueryObject", {Handle, 1, Output, 128, LengthOutput}),
            0xc000017cu);
  EXPECT_EQ(get(LengthOutput, 4), 0u);
  EXPECT_EQ(bytes(Output, 128), std::vector<uint8_t>(128, 0xa5));
  EXPECT_EQ(objectCall("ZwClose", {Handle}), 0u);
  EXPECT_NE(objectCall("ObGetObjectType", {Body}), 0u);
  objectCall("ObfDereferenceObject", {Body});
}

TEST_F(DriverKernelRegistry, DeletedPointerAndHandlesNeverBindToRecreatedKey) {
  const auto OldHandle = open(), Old = reference(OldHandle);
  EXPECT_EQ(call("ZwDeleteKey", {OldHandle}), 0u);
  object(Path);
  EXPECT_EQ(call("ZwCreateKey",
                 {HandleOutput, 0xf003f, Object, 0, 0, 0, Disposition}),
            0u);
  const auto NewHandle = get(HandleOutput), New = reference(NewHandle);
  EXPECT_NE(Old, New);
  EXPECT_EQ(get(Disposition, 4), 1u);
  EXPECT_EQ(objectCall("ZwClose", {OldHandle}), 0u);
  EXPECT_EQ(objectCall("ObOpenObjectByPointer",
                       {Old, 0x200, 0, 1, 0, 0, HandleOutput}),
            0u);
  const auto ReopenedOld = get(HandleOutput);
  EXPECT_EQ(query(ReopenedOld, 2, 16), 0xc000017cu);
  EXPECT_EQ(query(NewHandle, 2, 16), 0xc0000034u);
  objectCall("ObfDereferenceObject", {Old});
  EXPECT_EQ(objectCall("ZwClose", {ReopenedOld}), 0u);
  EXPECT_FALSE(objectFailure("ObGetObjectType", {Old}).empty());
  EXPECT_EQ(objectCall("ZwQueryObject", {NewHandle, 1, Output, 128, 0}), 0u);
  objectCall("ObfDereferenceObject", {New});
  EXPECT_EQ(objectCall("ZwClose", {NewHandle}), 0u);
}

TEST_F(DriverKernelRegistry,
       SharedHandleTypesAccessAndProcessScopeAreEnforced) {
  const auto Kernel = open(Path, 0x80000000);
  object(Path);
  put(Object + 24, 0x40, 4);
  EXPECT_EQ(call("ZwOpenKey", {HandleOutput, 0x80000000, Object}), 0u);
  const auto Local = get(HandleOutput);
  EXPECT_EQ(objectCall("ObReferenceObjectByHandle", {Local, 2, 0, 1, Data, 0}),
            0xc0000022u);
  EXPECT_EQ(
      objectCall("ObReferenceObjectByHandle", {Local, 1, 0, 1, Data, Output}),
      0u);
  EXPECT_EQ(get(Output, 4), 0u);
  EXPECT_EQ(get(Output + 4, 4), 0x20019u);
  objectCall("ObfDereferenceObject", {get(Data)});
  EXPECT_EQ(objectCall("ObReferenceObjectByHandle", {Kernel, 0, 0, 1, Data, 0}),
            0xc0000008u);
  success(Model->setUserRequestContext(true, 0x2000));
  EXPECT_EQ(query(Local, 2, 16), 0xc0000008u);
  EXPECT_EQ(objectCall("ZwClose", {Local}), 0xc0000008u);
  EXPECT_EQ(query(Kernel, 2, 16), 0u);
  EXPECT_FALSE(failure("ZwOpenKey", {HandleOutput, 1, Object}).empty());
  success(Model->setUserRequestContext(false));
  object("\\Driver");
  EXPECT_EQ(objectCall("ZwOpenDirectoryObject", {HandleOutput, 1, Object}), 0u);
  const auto Directory = get(HandleOutput);
  EXPECT_EQ(query(Directory, 2, 16), 0xc0000024u);
  object("Child", Directory);
  EXPECT_EQ(call("ZwOpenKey", {HandleOutput, 1, Object}), 0xc0000024u);
  EXPECT_EQ(objectCall("ZwQuerySymbolicLinkObject", {Kernel, ValueName, 0}),
            0xc0000024u);
  EXPECT_EQ(objectCall("ZwClose", {Directory}), 0u);
  EXPECT_EQ(objectCall("ZwClose", {Kernel}), 0u);
  EXPECT_EQ(objectCall("ZwClose", {Local}), 0u);
}

TEST_F(DriverKernelRegistry, FailedObjectOutputsDoNotPublishReferencesOrKeys) {
  constexpr auto ReadOnly = Scratch + 0x8000;
  success(Memory->protect(ReadOnly, 4096, Read));
  put(Disposition, 0xfeed, 4);
  object("\\Registry\\Machine\\Software\\NotPublished");
  EXPECT_FALSE(
      failure("ZwCreateKey", {ReadOnly, 1, Object, 0, 0, 0, Disposition})
          .empty());
  EXPECT_EQ(get(Disposition, 4), 0xfeedu);
  EXPECT_FALSE(failure("ZwCreateKey",
                       {Scratch + 0xfffc, 1, Object, 0, 0, 0, Disposition})
                   .empty());
  EXPECT_EQ(get(Disposition, 4), 0xfeedu);
  EXPECT_FALSE(Registry->hasOpenHandles());
  const auto Handle = open();
  EXPECT_EQ(Handle, 0x60000000u);
  EXPECT_FALSE(objectFailure("ObReferenceObjectByHandle",
                             {Handle, 0, 0, 0, Data, ReadOnly})
                   .empty());
  const auto Body = reference(Handle);
  put(LengthOutput, 0xfeed, 4);
  EXPECT_FALSE(
      objectFailure("ObQueryNameString", {Body, ReadOnly, 128, LengthOutput})
          .empty());
  EXPECT_EQ(get(LengthOutput, 4), 0xfeedu);
  EXPECT_FALSE(objectFailure("ObQueryNameString",
                             {Body, Scratch + 0xfff0, 128, LengthOutput})
                   .empty());
  EXPECT_EQ(get(LengthOutput, 4), 0xfeedu);
  EXPECT_FALSE(objectFailure("ObOpenObjectByPointer",
                             {Body, 0x200, 0, 1, 0, 0, ReadOnly})
                   .empty());
  EXPECT_EQ(objectCall("ZwClose", {Handle}), 0u);
  objectCall("ObfDereferenceObject", {Body});
  EXPECT_FALSE(objectFailure("ObGetObjectType", {Body}).empty());
  EXPECT_FALSE(Registry->hasOpenHandles());
  success(Registry->initialize(std::vector<DriverRegistryKey>{}));
}

TEST_F(DriverKernelRegistry,
       PointerHeldBodiesAndClonedHandlesHaveBoundedCapacity) {
  const auto Handle = open();
  const auto Body = reference(Handle);
  for (size_t I = 1; I < MaxRegistryHandles; ++I)
    EXPECT_EQ(objectCall("ObOpenObjectByPointer",
                         {Body, 0x200, 0, 1, 0, 0, HandleOutput}),
              0u);
  EXPECT_NE(objectFailure("ObOpenObjectByPointer",
                          {Body, 0x200, 0, 1, 0, 0, HandleOutput})
                .find("limit"),
            std::string::npos);
  for (size_t I = 0; I < MaxRegistryHandles; ++I)
    EXPECT_EQ(objectCall("ZwClose", {Handle + I * 4}), 0u);
  EXPECT_FALSE(Registry->hasOpenHandles());
  auto Reset = Registry->initialize(std::vector<DriverRegistryKey>{});
  ASSERT_TRUE(bool(Reset));
  EXPECT_NE(llvm::toString(std::move(Reset)).find("key objects"),
            std::string::npos);
  for (size_t I = 1; I < MaxRegistryHandles; ++I) {
    const auto H = open();
    reference(H);
    EXPECT_EQ(objectCall("ZwClose", {H}), 0u);
  }
  object(Path);
  EXPECT_NE(
      failure("ZwOpenKey", {HandleOutput, 1, Object}).find("key object count"),
      std::string::npos);
  objectCall("ObfDereferenceObject", {Body});
  const auto New = open();
  EXPECT_EQ(objectCall("ZwClose", {New}), 0u);
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

TEST_F(DriverKernelRegistry,
       EnumerationMatchesNativePartialOutputAndStatusMatrix) {
#define NEVERD_NATIVE_ENUM_METADATA(Name, Time, Values, MaxName, MaxData)      \
  const std::string NativeName = Name;                                         \
  const uint64_t NativeTime = Time;                                            \
  const unsigned ValueCount = Values, MaxValueNameBytes = MaxName,             \
                 MaxValueDataBytes = MaxData;
#define NEVERD_NATIVE_ENUM_CASE(Class, Index, Capacity, Status, Length, Hex)
#include "RegistryEnumerationNativeCases.def"
#undef NEVERD_NATIVE_ENUM_CASE
#undef NEVERD_NATIVE_ENUM_METADATA
  DriverRegistryKey Parent{Path, {}};
  Parent.SubkeyOrder = std::vector<std::string>{NativeName};
  DriverRegistryKey Child{std::string(Path) + "\\" + NativeName, {}};
  Child.Information = DriverRegistryKeyInformation{NativeTime, ""};
  Child.SubkeyOrder = std::vector<std::string>{};
  // These owned values reproduce only observed aggregate lengths/counts;
  // their names/data are synthetic, never read from the host registry.
  for (unsigned I = 0; I < ValueCount; ++I)
    Child.Values.push_back({std::string(MaxValueNameBytes / 2, char('A' + I)),
                            3, std::vector<uint8_t>(MaxValueDataBytes)});
  struct NativeCase {
    uint32_t Class, Index, Capacity, Status, Length;
    const char *Hex;
  };
  static const NativeCase Cases[] = {
#define NEVERD_NATIVE_ENUM_METADATA(Name, Time, Values, MaxName, MaxData)
#define NEVERD_NATIVE_ENUM_CASE(Class, Index, Capacity, Status, Length, Hex)   \
  {Class, Index, Capacity, Status, Length, Hex},
#include "RegistryEnumerationNativeCases.def"
#undef NEVERD_NATIVE_ENUM_CASE
#undef NEVERD_NATIVE_ENUM_METADATA
  };
  initialize({Parent, Child});
  const auto Handle = open(Path, 8);
  for (const auto &C : Cases) {
    SCOPED_TRACE(std::to_string(C.Class) + ":" + std::to_string(C.Index) + ":" +
                 std::to_string(C.Capacity));
    success(Memory->write(Output, std::vector<uint8_t>(2048, 0xa5)));
    put(LengthOutput, 0xa5a5a5a5, 4);
    EXPECT_EQ(call("ZwEnumerateKey", {Handle, C.Index, C.Class, Output,
                                      C.Capacity, LengthOutput}),
              C.Status);
    EXPECT_EQ(get(LengthOutput, 4), C.Length);
    EXPECT_EQ(llvm::toHex(bytes(Output, 128), true), C.Hex);
  }
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
}
TEST_F(DriverKernelRegistry,
       EnumerationRequiresObservedOrderAndOnlyNeedsTimestampForOutput) {
  auto Keys = *test::registryEnumerationOptions().Registry;
  const auto Root = Keys[0].Path;
  Keys[0].SubkeyOrder.reset();
  initialize(Keys);
  const auto H = open(Root, 8);
  EXPECT_NE(failure("ZwEnumerateKey", {H, 0, 0, 0, 0, LengthOutput})
                .find("subkey_order"),
            std::string::npos);
  put(LengthOutput, 0xa5a5a5a5, 4);
  EXPECT_EQ(call("ZwEnumerateKey", {H, 2, 0, 0, 0, LengthOutput}), 0x8000001au);
  EXPECT_EQ(get(LengthOutput, 4), 0xa5a5a5a5u);
  call("ZwClose", {H});
  Keys[0].SubkeyOrder = std::vector<std::string>{"Zulu", "Alpha"};
  Keys[2].Information.reset();
  initialize(Keys);
  const auto Known = open(Root, 8);
  EXPECT_EQ(call("ZwEnumerateKey", {Known, 0, 0, 0, 0, LengthOutput}),
            0xc0000023u);
  EXPECT_EQ(get(LengthOutput, 4), 24u);
  EXPECT_NE(failure("ZwEnumerateKey", {Known, 0, 0, Output, 64, LengthOutput})
                .find("information"),
            std::string::npos);
  EXPECT_EQ(call("ZwEnumerateKey", {Known, 1, 0, Output, 64, LengthOutput}),
            0u);
  EXPECT_EQ(get(Output), 0x1122334455667788u);
  call("ZwClose", {Known});
}
TEST_F(DriverKernelRegistry,
       EnumerationInvalidatesChangedOrderAndMetadataWithoutGuessing) {
  auto Keys = *test::registryEnumerationOptions().Registry;
  const auto Root = Keys[0].Path;
  initialize(Keys);
  const auto H = open(Root);
  const auto Alpha = open(Root + "\\Alpha");
  unicode(ValueName, "Mode");
  put(Data, 9, 4);
  EXPECT_EQ(call("ZwSetValueKey", {Alpha, ValueName, 0, 4, Data, 4}), 0u);
  EXPECT_NE(failure("ZwEnumerateKey", {H, 1, 0, Output, 64, LengthOutput})
                .find("information"),
            std::string::npos);
  EXPECT_EQ(call("ZwEnumerateKey", {H, 0, 0, Output, 64, LengthOutput}), 0u);
  EXPECT_EQ(get(Output), UINT64_C(0x8000000000000001));
  EXPECT_EQ(call("ZwDeleteKey", {Alpha}), 0u);
  EXPECT_EQ(call("ZwClose", {Alpha}), 0u);
  EXPECT_EQ(call("ZwEnumerateKey", {H, 1, 0, Output, 64, LengthOutput}),
            0x8000001au);
  EXPECT_EQ(call("ZwEnumerateKey", {H, 0, 0, Output, 64, LengthOutput}), 0u);
  EXPECT_EQ(get(Output), UINT64_C(0x8000000000000001));
  object("Beta", H);
  EXPECT_EQ(call("ZwCreateKey",
                 {HandleOutput, 0xf003f, Object, 0, 0, 0, Disposition}),
            0u);
  const auto Beta = get(HandleOutput);
  EXPECT_NE(failure("ZwEnumerateKey", {H, 1, 0, 0, 0, LengthOutput})
                .find("subkey_order"),
            std::string::npos);
  EXPECT_EQ(call("ZwEnumerateKey", {H, 2, 0, 0, 0, LengthOutput}), 0x8000001au);
  auto Snapshot = Registry->snapshot();
  ASSERT_TRUE(Snapshot);
  bool Found = false;
  for (const auto &K : *Snapshot)
    if (K.Path == Root) {
      Found = true;
      EXPECT_FALSE(K.SubkeyOrder);
    }
  EXPECT_TRUE(Found);
  call("ZwClose", {Beta});
  call("ZwClose", {H});
}
TEST_F(DriverKernelRegistry,
       EnumerationChecksCapabilitiesAndPreflightsAllOutputStores) {
  auto Keys = *test::registryEnumerationOptions().Registry;
  const auto Root = Keys[0].Path;
  initialize(Keys);
  const auto Limited = open(Root, 1);
  put(LengthOutput, 0xa5a5a5a5, 4);
  EXPECT_EQ(call("ZwEnumerateKey", {Limited, 0, 0, Output, 64, LengthOutput}),
            0xc0000022u);
  EXPECT_EQ(get(LengthOutput, 4), 0xa5a5a5a5u);
  call("ZwClose", {Limited});
  EXPECT_EQ(call("ZwEnumerateKey", {0, 0, 3, Output, 64, LengthOutput}),
            0xc000000du);
  const auto H = open(Root, 8);
  EXPECT_FALSE(
      failure("ZwEnumerateKey", {H, 0, 0, 0, 64, LengthOutput}).empty());
  EXPECT_EQ(get(LengthOutput, 4), 0xa5a5a5a5u);
  EXPECT_FALSE(
      failure("ZwEnumerateKey", {H, 0, 0, Output, 64, Output + 4}).empty());
  success(Memory->protect(Output, 4096, Read));
  EXPECT_FALSE(
      failure("ZwEnumerateKey", {H, 0, 0, Output, 64, LengthOutput}).empty());
  EXPECT_EQ(get(LengthOutput, 4), 0xa5a5a5a5u);
  success(Memory->protect(Output, 4096, Read | Write));
  EXPECT_EQ(call("ZwEnumerateKey",
                 {H, UINT64_C(0x100000000), 0, Output, 64, LengthOutput}),
            0u);
  call("ZwClose", {H});
}
} // namespace
} // namespace neverd::emulation
