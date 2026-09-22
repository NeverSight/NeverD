//===- UnicornMMIOTests.cpp - Device transaction boundaries ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Real CPU and host API access must share checked MMIO transaction semantics.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"

#include "neverd/emulation/DriverProfile.h"

#include <array>
#include <stdexcept>

namespace neverd::emulation {
namespace {
constexpr uint64_t Code = 0x10000;
constexpr uint64_t Device = 0x30000;
constexpr uint64_t Page = profile::PageSize;

llvm::Error rejected(const char *Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

class DriverUnicornMMIO : public ::testing::Test {
protected:
  std::unique_ptr<UnicornBackend> CPU;
  unsigned Reads = 0, Writes = 0, Validations = 0;
  uint64_t Value = 0x89abcdef;
  uint64_t LastOffset = 0;
  unsigned LastWidth = 0;
  BackendHooks Observers;

  void SetUp() override {
    auto Backend = UnicornBackend::create(5 * Page);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    CPU = std::move(*Backend);
    ASSERT_EQ(llvm::toString(CPU->map(Code, Page, Read | Write | Execute)), "");
  }

  GuestMMIOCallbacks callbacks() {
    return {[&](uint64_t, uint64_t, bool) {
              ++Validations;
              return llvm::Error::success();
            },
            [&](uint64_t Offset, unsigned Width) -> llvm::Expected<uint64_t> {
              ++Reads;
              LastOffset = Offset;
              LastWidth = Width;
              return Value;
            },
            [&](uint64_t Offset, unsigned Width, uint64_t Input) {
              ++Writes;
              LastOffset = Offset;
              LastWidth = Width;
              Value = Input;
              return llvm::Error::success();
            }};
  }

  void map(uint64_t Address = Device, uint64_t Size = Page) {
    ASSERT_EQ(llvm::toString(CPU->mapMMIO(Address, Size, callbacks())), "");
  }

  llvm::Error execute(llvm::ArrayRef<uint8_t> Bytes,
                      uint64_t Address = Device) {
    if (auto E = CPU->write(Code, Bytes))
      return E;
    if (auto E = CPU->setReg(X64Register::AX, Address))
      return E;
    auto Hooks = Observers;
    Hooks.Instruction = [&, End = Code + Bytes.size()](uint64_t PC, uint32_t) {
      if (PC == End)
        CPU->stop();
    };
    if (auto E = CPU->installHooks(std::move(Hooks)))
      return E;
    return CPU->run(Code, 1000000);
  }

  uint64_t read(uint64_t Address, unsigned Width) {
    auto Result = CPU->readInteger(Address, Width);
    if (!Result) {
      ADD_FAILURE() << llvm::toString(Result.takeError());
      return 0;
    }
    return *Result;
  }
};

struct ScalarCase {
  unsigned Width;
  bool IsWrite;
};
class DriverUnicornMMIOScalar
    : public DriverUnicornMMIO,
      public ::testing::WithParamInterface<ScalarCase> {};

TEST_P(DriverUnicornMMIOScalar, RealCPUUsesOneExactTransaction) {
  map();
  const auto Case = GetParam();
  const uint64_t Mask = (uint64_t(1) << (8 * Case.Width)) - 1;
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::CX, 0x11223344)), "");
  const std::array<uint8_t, 2> Read1{0x8a, 0x08}, Read4{0x8b, 0x08};
  const std::array<uint8_t, 2> Write1{0x88, 0x08}, Write4{0x89, 0x08};
  const std::array<uint8_t, 3> Read2{0x66, 0x8b, 0x08};
  const std::array<uint8_t, 3> Write2{0x66, 0x89, 0x08};
  llvm::ArrayRef<uint8_t> Bytes = Case.IsWrite ? llvm::ArrayRef<uint8_t>(Write4)
                                               : llvm::ArrayRef<uint8_t>(Read4);
  if (Case.Width == 1)
    Bytes = Case.IsWrite ? llvm::ArrayRef<uint8_t>(Write1)
                         : llvm::ArrayRef<uint8_t>(Read1);
  if (Case.Width == 2)
    Bytes = Case.IsWrite ? llvm::ArrayRef<uint8_t>(Write2)
                         : llvm::ArrayRef<uint8_t>(Read2);
  ASSERT_EQ(llvm::toString(execute(Bytes, Device + 4)), "");
  EXPECT_EQ(Reads, Case.IsWrite ? 0u : 1u);
  EXPECT_EQ(Writes, Case.IsWrite ? 1u : 0u);
  EXPECT_EQ(LastOffset, 4u);
  EXPECT_EQ(LastWidth, Case.Width);
  if (Case.IsWrite) {
    EXPECT_EQ(Value, 0x11223344u & Mask);
  } else {
    auto CX = CPU->reg(X64Register::CX);
    ASSERT_TRUE(bool(CX)) << llvm::toString(CX.takeError());
    EXPECT_EQ(*CX & Mask, Value & Mask);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Widths, DriverUnicornMMIOScalar,
    ::testing::Values(ScalarCase{1, false}, ScalarCase{2, false},
                      ScalarCase{4, false}, ScalarCase{1, true},
                      ScalarCase{2, true}, ScalarCase{4, true}));

TEST_F(DriverUnicornMMIO, HostScalarsUseCallbacksWithoutCPUObservers) {
  map();
  unsigned Observed = 0;
  BackendHooks Hooks;
  Hooks.Read = [&](uint64_t, uint32_t) { ++Observed; };
  Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ++Observed; };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Hooks))), "");
  for (unsigned Width : {1, 2, 4}) {
    ASSERT_EQ(llvm::toString(CPU->writeInteger(Device + 8, 0x76543210, Width)),
              "");
    EXPECT_EQ(read(Device + 8, Width),
              0x76543210u & ((uint64_t(1) << (Width * 8)) - 1));
  }
  EXPECT_EQ(Reads, 3u);
  EXPECT_EQ(Writes, 3u);
  EXPECT_EQ(Observed, 0u);
}

class DriverUnicornMMIOAccess : public DriverUnicornMMIO,
                                public ::testing::WithParamInterface<bool> {};

TEST_P(DriverUnicornMMIOAccess, CPUWideAccessFailsBeforeSplitEffects) {
  map();
  unsigned Observed = 0;
  Observers.Read = [&](uint64_t, uint32_t) { ++Observed; };
  Observers.Write = [&](uint64_t, uint32_t, uint64_t) { ++Observed; };
  const std::array<uint8_t, 3> Read{0x48, 0x8b, 0x08};
  const std::array<uint8_t, 3> Write{0x48, 0x89, 0x08};
  auto Error = llvm::toString(execute(GetParam() ? Write : Read));
  EXPECT_NE(Error.find("aligned 1, 2 or 4"), std::string::npos);
  EXPECT_EQ(Reads + Writes + Validations + Observed, 0u);
  EXPECT_FALSE(CPU->hasMemoryFault());
  EXPECT_TRUE(CPU->hasDeviceError());
  EXPECT_EQ(llvm::toString(CPU->run(Code, 1000000)), Error);
}

TEST_P(DriverUnicornMMIOAccess, CPUUnalignedAccessCannotTouchNeighbors) {
  map();
  const std::array<uint8_t, 2> Read{0x8b, 0x08};
  const std::array<uint8_t, 2> Write{0x89, 0x08};
  EXPECT_NE(llvm::toString(execute(GetParam() ? Write : Read, Device + 1))
                .find("aligned"),
            std::string::npos);
  EXPECT_EQ(Reads + Writes + Validations, 0u);
}

TEST_P(DriverUnicornMMIOAccess, HostBulkCannotBypassOriginalWidth) {
  map();
  std::array<uint8_t, 8> Bytes{};
  auto E = GetParam() ? CPU->write(Device, Bytes) : CPU->read(Device, Bytes);
  EXPECT_NE(llvm::toString(std::move(E)).find("aligned 1, 2 or 4"),
            std::string::npos);
  EXPECT_EQ(Reads + Writes + Validations, 0u);
}

TEST_P(DriverUnicornMMIOAccess, ExternalObserverStopSuppressesDeviceEffects) {
  map();
  unsigned Observed = 0;
  Observers.Read = [&](uint64_t, uint32_t) {
    ++Observed;
    CPU->stop();
  };
  Observers.Write = [&](uint64_t, uint32_t, uint64_t) {
    ++Observed;
    CPU->stop();
  };
  const std::array<uint8_t, 2> Read{0x8b, 0x08};
  const std::array<uint8_t, 2> Write{0x89, 0x08};
  ASSERT_EQ(llvm::toString(execute(GetParam() ? Write : Read)), "");
  EXPECT_EQ(Observed, 1u);
  EXPECT_EQ(Reads + Writes, 0u);
  // A normal API/observer stop may continue and does not poison device state.
  Observers = {};
  ASSERT_EQ(llvm::toString(execute(GetParam() ? Write : Read)), "");
  EXPECT_EQ(Reads, GetParam() ? 0u : 1u);
  EXPECT_EQ(Writes, GetParam() ? 1u : 0u);
}

TEST_P(DriverUnicornMMIOAccess, CallbackFailureIsNotSuccessfulZero) {
  auto Callbacks = callbacks();
  Callbacks.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    return rejected("device read rejected");
  };
  Callbacks.Write = [&](uint64_t, unsigned, uint64_t) {
    return rejected("device write rejected");
  };
  ASSERT_EQ(llvm::toString(CPU->mapMMIO(Device, Page, std::move(Callbacks))),
            "");
  const std::array<uint8_t, 2> Read{0x8b, 0x08};
  const std::array<uint8_t, 2> Write{0x89, 0x08};
  EXPECT_EQ(llvm::toString(execute(GetParam() ? Write : Read)),
            GetParam() ? "device write rejected" : "device read rejected");
  EXPECT_EQ(Reads + Writes, 0u);
}

INSTANTIATE_TEST_SUITE_P(Direction, DriverUnicornMMIOAccess, ::testing::Bool());

TEST_F(DriverUnicornMMIO, RegisterPolicyRejectsCPUAccessBeforeObserver) {
  auto Callbacks = callbacks();
  Callbacks.Validate = [](uint64_t Offset, uint64_t, bool IsWrite) {
    return IsWrite || Offset != 4 ? rejected("register policy denied")
                                  : llvm::Error::success();
  };
  ASSERT_EQ(llvm::toString(CPU->mapMMIO(Device, Page, std::move(Callbacks))),
            "");
  EXPECT_EQ(read(Device + 4, 4), Value);
  unsigned Observed = 0;
  Observers.Write = [&](uint64_t, uint32_t, uint64_t) { ++Observed; };
  EXPECT_EQ(llvm::toString(execute({0x89, 0x08}, Device + 4)),
            "register policy denied");
  EXPECT_EQ(Observed + Writes, 0u);
  EXPECT_EQ(Reads, 1u);
}

TEST_F(DriverUnicornMMIO, HostCallbackFailurePreservesFirstError) {
  auto Callbacks = callbacks();
  Callbacks.Read = [](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    return rejected("peripheral timeout");
  };
  ASSERT_EQ(llvm::toString(CPU->mapMMIO(Device, Page, std::move(Callbacks))),
            "");
  auto Result = CPU->readInteger(Device, 4);
  ASSERT_FALSE(bool(Result));
  EXPECT_EQ(llvm::toString(Result.takeError()), "peripheral timeout");
  EXPECT_EQ(llvm::toString(CPU->writeInteger(Device, 1, 4)),
            "peripheral timeout");
  EXPECT_EQ(Writes, 0u);
  EXPECT_FALSE(CPU->hasMemoryFault());
}

TEST_F(DriverUnicornMMIO, HostValidationExceptionCannotEscape) {
  auto Callbacks = callbacks();
  Callbacks.Validate = [](uint64_t, uint64_t, bool) -> llvm::Error {
    throw std::runtime_error("validation exception");
  };
  ASSERT_EQ(llvm::toString(CPU->mapMMIO(Device, Page, std::move(Callbacks))),
            "");
  EXPECT_EQ(llvm::toString(CPU->writeInteger(Device, 1, 4)),
            "exception in emulator hook");
  EXPECT_EQ(Reads + Writes, 0u);
}

TEST_F(DriverUnicornMMIO, CPUReadCallbackExceptionCannotEscape) {
  auto Callbacks = callbacks();
  Callbacks.Read = [](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    throw std::runtime_error("read exception");
  };
  ASSERT_EQ(llvm::toString(CPU->mapMMIO(Device, Page, std::move(Callbacks))),
            "");
  EXPECT_EQ(llvm::toString(execute({0x8b, 0x08})),
            "exception in emulator hook");
  EXPECT_FALSE(CPU->hasDeviceError());
}

TEST_F(DriverUnicornMMIO, HostWriteCallbackExceptionCannotEscape) {
  auto Callbacks = callbacks();
  Callbacks.Write = [](uint64_t, unsigned, uint64_t) -> llvm::Error {
    throw std::runtime_error("write exception");
  };
  ASSERT_EQ(llvm::toString(CPU->mapMMIO(Device, Page, std::move(Callbacks))),
            "");
  EXPECT_EQ(llvm::toString(CPU->writeInteger(Device, 1, 4)),
            "exception in emulator hook");
}

TEST_F(DriverUnicornMMIO, ObserverExceptionSuppressesPendingDeviceWrite) {
  map();
  Observers.Write = [](uint64_t, uint32_t, uint64_t) {
    throw std::runtime_error("observer exception");
  };
  EXPECT_EQ(llvm::toString(execute({0x89, 0x08})),
            "exception in emulator hook");
  EXPECT_EQ(Writes, 0u);
}

TEST_F(DriverUnicornMMIO, CallbackCannotDestroyItsExecutingMapping) {
  auto Callbacks = callbacks();
  Callbacks.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    EXPECT_NE(llvm::toString(CPU->unmapMMIO(Device, Page)).find("callback"),
              std::string::npos);
    return 7;
  };
  ASSERT_EQ(llvm::toString(CPU->mapMMIO(Device, Page, std::move(Callbacks))),
            "");
  EXPECT_EQ(read(Device, 4), 7u);
  EXPECT_EQ(read(Device, 4), 7u);
  ASSERT_EQ(llvm::toString(CPU->unmapMMIO(Device, Page)), "");
}

TEST_F(DriverUnicornMMIO, RealREPBufferReadPreservesIndividualTransactions) {
  map();
  ASSERT_EQ(llvm::toString(CPU->map(Device + Page, Page, Read | Write)), "");
  // MOV rsi, Device; MOV rdi, Device+Page; MOV ecx, 2; REP MOVSD.
  const std::array<uint8_t, 27> Bytes{
      0x48, 0xbe, 0, 0, 3, 0, 0,    0, 0, 0, 0x48, 0xbf, 0,   0x10,
      3,    0,    0, 0, 0, 0, 0xb9, 2, 0, 0, 0,    0xf3, 0xa5};
  ASSERT_EQ(llvm::toString(execute(Bytes)), "");
  EXPECT_EQ(Reads, 2u);
  EXPECT_EQ(Writes, 0u);
  EXPECT_EQ(LastOffset, 4u);
  EXPECT_EQ(LastWidth, 4u);
  EXPECT_EQ(read(Device + Page, 4), Value);
  EXPECT_EQ(read(Device + Page + 4, 4), Value);
}

TEST_F(DriverUnicornMMIO, AdjacentMappingCrossingFailsBeforeEitherBank) {
  map();
  map(Device + Page);
  EXPECT_NE(llvm::toString(execute({0x89, 0x08}, Device + Page - 2))
                .find("mapping boundary"),
            std::string::npos);
  EXPECT_EQ(Reads + Writes + Validations, 0u);
}

TEST_F(DriverUnicornMMIO, RAMToMMIOBulkCrossingCannotPartiallyWriteBank) {
  ASSERT_EQ(llvm::toString(CPU->map(Device - Page, Page, Read | Write)), "");
  map();
  std::array<uint8_t, 8> Bytes{};
  EXPECT_NE(
      llvm::toString(CPU->write(Device - 4, Bytes)).find("mapping boundary"),
      std::string::npos);
  EXPECT_EQ(Reads + Writes + Validations, 0u);
}

TEST_F(DriverUnicornMMIO, MMIOToRAMCPUAccessCannotPartiallyReadBank) {
  map();
  ASSERT_EQ(llvm::toString(CPU->map(Device + Page, Page, Read | Write)), "");
  EXPECT_NE(llvm::toString(execute({0x8b, 0x08}, Device + Page - 2))
                .find("mapping boundary"),
            std::string::npos);
  EXPECT_EQ(Reads + Writes + Validations, 0u);
}

TEST_F(DriverUnicornMMIO, AliasesShareStateAcrossCPUContextRestoration) {
  map();
  map(Device + Page);
  auto Context = CPU->saveContext();
  ASSERT_TRUE(bool(Context)) << llvm::toString(Context.takeError());
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Device, 0x12345678, 4)), "");
  ASSERT_EQ(llvm::toString(CPU->restoreContext(**Context)), "");
  EXPECT_EQ(read(Device + Page, 4), 0x12345678u);
  ASSERT_EQ(llvm::toString(CPU->unmapMMIO(Device, Page)), "");
  EXPECT_EQ(read(Device + Page, 4), 0x12345678u);
}

TEST_F(DriverUnicornMMIO, ExactUnmapRetiresCallbacksAndReturnsBudget) {
  auto Lifetime = std::make_shared<int>(1);
  std::weak_ptr<int> Weak = Lifetime;
  auto Callbacks = callbacks();
  Callbacks.Validate = [Lifetime](uint64_t, uint64_t, bool) {
    return llvm::Error::success();
  };
  Lifetime.reset();
  ASSERT_EQ(
      llvm::toString(CPU->mapMMIO(Device, 4 * Page, std::move(Callbacks))), "");
  EXPECT_NE(llvm::toString(CPU->mapMMIO(Device + 8 * Page, Page, callbacks()))
                .find("memory limit"),
            std::string::npos);
  EXPECT_NE(llvm::toString(CPU->unmapMMIO(Device, Page)).find("exact"),
            std::string::npos);
  EXPECT_FALSE(Weak.expired());
  EXPECT_EQ(read(Device, 4), Value);
  ASSERT_EQ(llvm::toString(CPU->unmapMMIO(Device, 4 * Page)), "");
  EXPECT_TRUE(Weak.expired());
  ASSERT_EQ(
      llvm::toString(CPU->mapMMIO(Device + 8 * Page, 4 * Page, callbacks())),
      "");
  auto Stale = CPU->readInteger(Device, 4);
  ASSERT_FALSE(bool(Stale));
  llvm::consumeError(Stale.takeError());
  ASSERT_TRUE(CPU->fault());
  EXPECT_EQ(CPU->fault()->Kind, BackendFaultKind::UnmappedMemory);
}

TEST_F(DriverUnicornMMIO, MappingShortageHasDistinctRecoverableErrorType) {
  map(Device, 4 * Page);
  bool Shortage = false;
  auto Error = CPU->mapMMIO(Device + 8 * Page, Page, callbacks());
  Error =
      llvm::handleErrors(std::move(Error), [&](const GuestMemoryLimitError &) {
        Shortage = true;
      });
  EXPECT_EQ(llvm::toString(std::move(Error)), "");
  EXPECT_TRUE(Shortage);
  EXPECT_FALSE(CPU->hasDeviceError());
  EXPECT_EQ(read(Device, 4), Value);
  ASSERT_EQ(llvm::toString(CPU->unmapMMIO(Device, 4 * Page)), "");
  map(Device + 8 * Page);
  EXPECT_EQ(read(Device + 8 * Page, 4), Value);

  auto Invalid = CPU->mapMMIO(Device + 1, Page, callbacks());
  EXPECT_FALSE(Invalid.isA<GuestMemoryLimitError>());
  EXPECT_NE(llvm::toString(std::move(Invalid)).find("invalid"),
            std::string::npos);
  auto RAM = CPU->map(Device, 4 * Page, Read | Write);
  EXPECT_FALSE(RAM.isA<GuestMemoryLimitError>());
  EXPECT_NE(llvm::toString(std::move(RAM)).find("memory limit"),
            std::string::npos);
}

TEST_F(DriverUnicornMMIO, DevicePagesStayNonExecutable) {
  map();
  EXPECT_FALSE(CPU->executable(Device));
  EXPECT_NE(
      llvm::toString(CPU->protect(Device, Page, Read | Execute)).find("MMIO"),
      std::string::npos);
  auto E = CPU->run(Device, 1000000);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  ASSERT_TRUE(CPU->fault());
  EXPECT_EQ(CPU->fault()->Kind, BackendFaultKind::Protection);
  EXPECT_EQ(CPU->fault()->Access, BackendAccessKind::Execute);
  EXPECT_EQ(Reads + Writes + Validations, 0u);
}

TEST_F(DriverUnicornMMIO, FailedMappingDoesNotReserveAddressOrCallbacks) {
  EXPECT_NE(llvm::toString(CPU->mapMMIO(Device, Page, {})).find("callbacks"),
            std::string::npos);
  EXPECT_NE(llvm::toString(CPU->mapMMIO(Device + 1, Page, callbacks()))
                .find("invalid"),
            std::string::npos);
  EXPECT_NE(
      llvm::toString(CPU->mapMMIO(Code, Page, callbacks())).find("overlapping"),
      std::string::npos);
  map();
  EXPECT_EQ(read(Device, 4), Value);
  EXPECT_NE(llvm::toString(CPU->unmapMMIO(Code, Page)).find("exact"),
            std::string::npos);
}
} // namespace
} // namespace neverd::emulation
