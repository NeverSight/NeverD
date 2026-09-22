//===- KernelFrameworkTestSupport.h - Shared guest framework fixture ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Deterministic byte storage and host allocation accounting for KMDF tests.
/// All driver/framework operations still execute the production model.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_EMULATION_KERNELFRAMEWORKTESTSUPPORT_H
#define NEVERD_UNITTESTS_EMULATION_KERNELFRAMEWORKTESTSUPPORT_H

#include "GuestMemory.h"
#include "gtest/gtest.h"
#include "windows/DriverImage.h"
#include "windows/KernelFramework.h"
#include "windows/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <set>

namespace neverd::emulation {
namespace framework_test {

inline llvm::Error failure(const char *Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

inline void success(llvm::Error Error) {
  if (Error)
    ADD_FAILURE() << llvm::toString(std::move(Error));
}

template <class T> T take(llvm::Expected<T> Result) {
  if (!Result) {
    ADD_FAILURE() << llvm::toString(Result.takeError());
    return {};
  }
  return std::move(*Result);
}

template <class T>
void expectError(llvm::Expected<T> Result, llvm::StringRef Text) {
  ASSERT_FALSE(bool(Result));
  const auto Message = llvm::toString(Result.takeError());
  EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
}

inline void expectError(llvm::Error Error, llvm::StringRef Text) {
  ASSERT_TRUE(bool(Error));
  const auto Message = llvm::toString(std::move(Error));
  EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
}

class FrameworkMemory final : public GuestMemory {
public:
  static constexpr uint64_t Base = 0x100000;
  std::vector<uint8_t> Bytes = std::vector<uint8_t>(0x200000);

  bool contains(uint64_t Address, uint64_t Size) const {
    return Address >= Base && Address - Base <= Bytes.size() &&
           Size <= Bytes.size() - (Address - Base);
  }
  llvm::Error map(uint64_t, uint64_t, unsigned) override {
    return failure("test mapping is fixed");
  }
  llvm::Error protect(uint64_t, uint64_t, unsigned) override {
    return failure("test mapping is fixed");
  }
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Output) override {
    if (!contains(Address, Output.size()))
      return failure("test read outside mapping");
    std::copy_n(Bytes.begin() + Address - Base, Output.size(), Output.begin());
    return llvm::Error::success();
  }
  llvm::Error write(uint64_t Address, llvm::ArrayRef<uint8_t> Input) override {
    if (!contains(Address, Input.size()))
      return failure("test write outside mapping");
    std::copy(Input.begin(), Input.end(), Bytes.begin() + Address - Base);
    return llvm::Error::success();
  }
};

class DriverKernelFramework : public ::testing::Test {
protected:
  static constexpr uint64_t Driver = FrameworkMemory::Base;
  static constexpr uint64_t Registry = Driver + 0x200;
  static constexpr uint64_t RegistryText = Driver + 0x300;
  static constexpr uint64_t Info = Driver + 0x800;
  static constexpr uint64_t Component = Driver + 0x900;
  static constexpr uint64_t TableSlot = Driver + 0xa00;
  static constexpr uint64_t GlobalsSlot = Driver + 0xa08;
  static constexpr uint64_t Minimum = Driver + 0xa10;
  static constexpr uint64_t Higher = Driver + 0xa18;
  static constexpr uint64_t FunctionCount = Driver + 0xa20;
  static constexpr uint64_t StructureCount = Driver + 0xa28;
  static constexpr uint64_t StructureTable = Driver + 0xa30;
  static constexpr uint64_t Config = Driver + 0xb00;
  static constexpr uint64_t DriverSlot = Driver + 0xb40;
  static constexpr uint64_t Output = Driver + 0xb48;
  static constexpr uint64_t ContextOutput = Driver + 0xb50;
  static constexpr uint64_t Attrs = Driver + 0xc00;
  static constexpr uint64_t Type = Driver + 0xd00;
  static constexpr uint64_t TypeOther = Driver + 0xd40;
  static constexpr uint64_t UnloadPC = 0x180001000;
  static constexpr uint64_t ParentCleanup = 0x180001100;
  static constexpr uint64_t ParentDestroy = 0x180001200;
  static constexpr uint64_t ChildCleanup = 0x180001300;
  static constexpr uint64_t ChildDestroy = 0x180001400;
  static constexpr uint64_t Sentinel = 0xfedcba9876543210;

  FrameworkMemory Memory;
  KernelExportRegistry Exports;
  uint64_t NextAllocation = Driver + 0x10000;
  size_t AllocationAttempts = 0;
  size_t FailAllocation = 0;
  uint64_t DenyWriteAt = 0;
  std::map<uint64_t, uint64_t> Allocations;
  std::set<uint64_t> Released;
  KernelFramework Model{
      Memory, Exports,
      [this](uint64_t Size) -> llvm::Expected<uint64_t> {
        if (++AllocationAttempts == FailAllocation)
          return failure("injected allocation failure");
        if (!Memory.contains(NextAllocation, Size))
          return failure("test allocation exhausted");
        const uint64_t Address = NextAllocation;
        NextAllocation += (Size + 15) & ~uint64_t(15);
        Allocations.emplace(Address, Size);
        return Address;
      },
      [this](uint64_t Address, uint32_t Size, bool Write) -> llvm::Error {
        if (!Memory.contains(Address, Size))
          return failure("test validation outside mapping");
        if (Write && DenyWriteAt >= Address && DenyWriteAt - Address < Size)
          return failure("injected write validation failure");
        return llvm::Error::success();
      },
      [this](uint64_t Address, uint64_t Size) -> llvm::Error {
        if (!Allocations.count(Address) || Allocations.at(Address) != Size ||
            !Released.insert(Address).second)
          return failure("invalid test allocation release");
        return llvm::Error::success();
      }};
  uint64_t Globals = 0;

  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    success(Memory.writeInteger(Address, Value, Width));
  }
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory.readInteger(Address, Width));
  }
  void utf16(uint64_t Address, llvm::StringRef Value) {
    for (size_t I = 0; I < Value.size(); ++I)
      put(Address + I * 2, uint8_t(Value[I]), 2);
    put(Address + Value.size() * 2, 0, 2);
  }
  void SetUp() override {
    success(Exports.initialize(DriverOptions{}));
    Model.configure(Driver, Registry, "NeverDFramework");
    constexpr llvm::StringLiteral Path = "\\Registry\\Machine\\NeverDFramework";
    utf16(RegistryText, Path);
    put(Registry, Path.size() * 2, 2);
    put(Registry + 2, Path.size() * 2 + 2, 2);
    put(Registry + 8, RegistryText);
    utf16(Component, "KmdfLibrary");
    put(Config, 32, 4);
    put(Config + 16, UnloadPC);
    put(Config + 24, 1, 4);
    initializeInfo();
  }
  void initializeInfo(bool Version2 = false) {
    success(Memory.write(Info, std::vector<uint8_t>(88)));
    put(Info, Version2 ? 88 : 48, 4);
    put(Info + 8, Component);
    put(Info + 16, 1, 4);
    put(Info + 20, 33, 4);
    put(Info + 28, 458, 4);
    put(Info + 32, TableSlot);
    put(TableSlot, Sentinel);
    put(GlobalsSlot, Sentinel);
    if (Version2) {
      put(Info + 48, Minimum);
      put(Info + 56, Higher);
      put(Info + 64, FunctionCount);
      put(Info + 72, StructureCount);
      put(Info + 80, StructureTable);
      put(Minimum, 33, 4);
      put(Higher, 1, 1);
      put(FunctionCount, 458, 4);
      put(StructureCount, 81, 4);
      put(StructureTable, Sentinel);
    }
  }
  llvm::Expected<uint64_t> loader(llvm::StringRef Name,
                                  std::initializer_list<uint64_t> Arguments) {
    const auto Address =
        take(Exports.bindImport({0, "WDFLDR.SYS", Name.str()}));
    const auto *Export = Exports.lookup(Address);
    if (!Export)
      return failure("missing loader test export");
    return Model.call(*Export, Arguments, 0);
  }
  void bind(bool Version2 = false) {
    initializeInfo(Version2);
    EXPECT_EQ(
        take(loader("WdfVersionBind", {Driver, Registry, Info, GlobalsSlot})),
        0u);
    Globals = get(GlobalsSlot);
    ASSERT_NE(Globals, Sentinel);
  }
  KernelExportRegistry::Export entry(llvm::StringRef Name) {
    const auto Address = take(Exports.insertFrameworkFunction(Globals, Name));
    const auto *Export = Exports.lookup(Address);
    if (!Export) {
      ADD_FAILURE() << "missing framework test export";
      return {};
    }
    return *Export;
  }
  llvm::Expected<uint64_t> invoke(llvm::StringRef Name,
                                  std::initializer_list<uint64_t> Arguments,
                                  uint8_t IRQL = 0) {
    return Model.call(entry(Name), Arguments, IRQL);
  }
  uint64_t createDriver(uint64_t Attributes = 0) {
    EXPECT_EQ(take(invoke("WdfDriverCreate", {Globals, Driver, Registry,
                                              Attributes, Config, DriverSlot})),
              0u);
    return get(DriverSlot);
  }
  void attributes(uint64_t Parent = 0, uint64_t TypeInfo = 0,
                  uint64_t Cleanup = 0, uint64_t Destroy = 0) {
    success(Memory.write(Attrs, std::vector<uint8_t>(56)));
    put(Attrs, 56, 4);
    put(Attrs + 8, Cleanup);
    put(Attrs + 16, Destroy);
    put(Attrs + 24, 1, 4);
    put(Attrs + 28, 1, 4);
    put(Attrs + 32, Parent);
    put(Attrs + 48, TypeInfo);
  }
  void type(uint64_t Address = Type, uint64_t Unique = 0) {
    success(Memory.write(Address, std::vector<uint8_t>(40)));
    put(Address, 40, 4);
    put(Address + 8, Address + 48);
    success(Memory.write(Address + 48, {'C', 't', 'x', 0}));
    put(Address + 16, 24);
    put(Address + 24, Unique);
  }
  uint64_t object(uint64_t Attributes = 0) {
    EXPECT_EQ(take(invoke("WdfObjectCreate", {Globals, Attributes, Output})),
              0u);
    return get(Output);
  }
  KernelFramework::GuestCall callback() {
    auto Call = Model.takeGuestCall();
    if (!Call) {
      ADD_FAILURE() << "expected a deferred guest callback";
      return {};
    }
    return *Call;
  }
  void finish(const KernelFramework::GuestCall &Call) {
    take(Model.finishGuestCall(Call.Token, 0));
  }
  std::vector<uint64_t> drain() {
    std::vector<uint64_t> PCs;
    while (auto Call = Model.takeGuestCall()) {
      PCs.push_back(Call->PC);
      if (PCs.size() > 32) {
        ADD_FAILURE() << "callback continuation did not terminate";
        break;
      }
      finish(*Call);
    }
    return PCs;
  }
  void unload() {
    const auto *Export =
        Exports.lookup(get(Driver + windows::DriverUnloadOffset));
    ASSERT_NE(Export, nullptr);
    EXPECT_EQ(take(Model.call(*Export, {Driver}, 0)), 0u);
  }
  size_t liveAllocations() const {
    return Allocations.size() - Released.size();
  }
};

} // namespace framework_test
} // namespace neverd::emulation

#endif
