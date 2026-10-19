//===- KernelModelTests.cpp - Windows initialization API contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows initialization API contracts.
///
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelAPINames.h"
#include "os/windows/kernel/KernelException.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelObjectTypes.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include <algorithm>
#include <initializer_list>
#include <string_view>

namespace neverd::emulation {
namespace {
namespace mutex_test {
#define NEVERD_SEH_MUTEX_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "fixtures/driver_seh_mutex.def"
#undef NEVERD_SEH_MUTEX_VALUE
} // namespace mutex_test

class DriverKernelModel : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;

  virtual DriverOptions modelOptions() { return {}; }
  void SetUp() override {
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(static_cast<bool>(Backend))
        << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    success(Model->initialize(Image, modelOptions()));
  }

  void success(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }

  uint64_t invoke(llvm::StringRef Name,
                  std::initializer_list<uint64_t> Arguments) {
    auto Value = Model->call(Name.str(), Arguments);
    if (!Value) {
      ADD_FAILURE() << Name.str() << ": " << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }

  std::string failure(llvm::StringRef Name,
                      std::initializer_list<uint64_t> Arguments) {
    auto Value = Model->call(Name.str(), Arguments);
    if (Value) {
      ADD_FAILURE() << Name.str() << " unexpectedly succeeded";
      return {};
    }
    auto Message = llvm::toString(Value.takeError());
    return Message;
  }

  std::string denied(uint64_t Address, uint32_t Size, bool IsWrite = false) {
    auto Error = Model->validateGuestAccess(Address, Size, IsWrite);
    if (!Error) {
      ADD_FAILURE() << "guest access unexpectedly allowed";
      return {};
    }
    return llvm::toString(std::move(Error));
  }

  uint64_t integer(uint64_t Address, unsigned Size = 8) {
    auto Value = Memory->readInteger(Address, Size);
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }

  std::vector<uint8_t> bytes(uint64_t Address, size_t Size) {
    std::vector<uint8_t> Bytes(Size);
    success(Memory->read(Address, Bytes));
    return Bytes;
  }

  void ascii(uint64_t Address, std::string_view Text) {
    std::vector<uint8_t> Bytes(Text.begin(), Text.end());
    Bytes.push_back(0);
    success(Memory->write(Address, Bytes));
  }

  void utf16(uint64_t Address, std::u16string_view Text) {
    std::vector<uint8_t> Bytes((Text.size() + 1) * 2);
    for (size_t I = 0; I < Text.size(); ++I) {
      Bytes[I * 2] = static_cast<uint8_t>(Text[I]);
      Bytes[I * 2 + 1] = static_cast<uint8_t>(Text[I] >> 8);
    }
    success(Memory->write(Address, Bytes));
  }

  void unicode(uint64_t Record, std::u16string_view Text) {
    // Construct the public x64 UNICODE_STRING ABI independently of the API
    // under test: byte lengths, then an aligned guest buffer pointer.
    utf16(Record + 32, Text);
    success(Memory->writeInteger(Record, Text.size() * 2, 2));
    success(Memory->writeInteger(Record + 2, (Text.size() + 1) * 2, 2));
    success(Memory->writeInteger(Record + 8, Record + 32, 8));
  }

  void checkDeviceOutputFailure(uint64_t Output, BackendFaultKind Kind) {
    const uint64_t Name = Scratch + 0x100;
    unicode(Name, u"\\Device\\Unpublished");
    ASSERT_EQ(invoke("IoCreateDevice",
                     {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch}),
              0u);
    const uint64_t First = integer(Scratch);
    EXPECT_NE(failure("IoCreateDevice",
                      {Model->driverObject(), 32, Name, 0x22, 0, 0, Output})
                  .find("guest write fault"),
              std::string::npos);
    EXPECT_EQ(integer(Model->driverObject() + windows::DriverDeviceHead),
              First);
    EXPECT_EQ(integer(First + windows::DeviceNext), 0u);
    success(Model->snapshot());
    ASSERT_EQ(Result.Devices.size(), 1u);
    EXPECT_EQ(Result.Devices[0].Address, First);
    EXPECT_TRUE(Result.Devices[0].Name.empty());
    const auto Fault = Memory->fault();
    ASSERT_TRUE(Fault);
    EXPECT_EQ(Fault->Kind, Kind);
    EXPECT_EQ(Fault->Address, Output);
    EXPECT_EQ(Fault->Size, 8u);
    EXPECT_EQ(Fault->Access, BackendAccessKind::Write);
  }
};

class DriverNativeKernelModel : public DriverKernelModel {
protected:
  DriverOptions modelOptions() override {
    DriverOptions O;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    return O;
  }
};

TEST_F(DriverKernelModel, StackQueryRequiresLiveActivation) {
  constexpr uint64_t Base = Scratch + 0x8000, Size = 0x2000;
  EXPECT_FALSE(failure("IoGetInitialStack", {}).empty());
  EXPECT_FALSE(failure("IoGetStackLimits", {Scratch, Scratch + 8}).empty());
  Model->enterExecution(Base);
  EXPECT_FALSE(failure("IoGetInitialStack", {}).empty());
  success(Model->activateStack(Base, Size));
  EXPECT_EQ(invoke("IoGetInitialStack", {}), Base + Size);
  invoke("IoGetStackLimits", {Scratch + 1, Scratch + 17});
  EXPECT_EQ(integer(Scratch + 1), Base);
  EXPECT_EQ(integer(Scratch + 17), Base + Size);
  EXPECT_FALSE(failure("IoGetInitialStack", {0}).empty());
  EXPECT_FALSE(failure("IoGetStackLimits", {Scratch}).empty());
  EXPECT_FALSE(failure("IoGetStackLimits", {Scratch, Scratch + 8, 0}).empty());
  success(Model->retireStack(Base, Size));
  EXPECT_FALSE(failure("IoGetInitialStack", {}).empty());
  EXPECT_FALSE(failure("IoGetStackLimits", {Scratch, Scratch + 8}).empty());
}

TEST_F(DriverKernelModel, StackQueryTracksNestedStackAndRestoration) {
  constexpr uint64_t Parent = Scratch + 0x8000, Child = Scratch + 0xc000;
  success(Model->activateStack(Parent, 0x2000));
  success(Model->activateStack(Child, 0x1000));
  Model->enterExecution(Parent, 0x1234);
  const auto Saved = Model->captureExecutionContext();
  success(Model->inheritExecutionContext(Child, Parent));
  Model->enterExecution(Child, 0x1234);
  EXPECT_EQ(invoke("IoGetInitialStack", {}), Child + 0x1000);
  invoke("IoGetStackLimits", {Scratch, Scratch + 8});
  EXPECT_EQ(integer(Scratch), Child);
  EXPECT_EQ(integer(Scratch + 8), Child + 0x1000);
  success(Model->restoreExecutionContext(Saved));
  EXPECT_EQ(invoke("IoGetInitialStack", {}), Parent + 0x2000);
  invoke("IoGetStackLimits", {Scratch + 8, Scratch});
  EXPECT_EQ(integer(Scratch + 8), Parent);
  EXPECT_EQ(integer(Scratch), Parent + 0x2000);
  success(Model->retireStack(Child, 0x1000));
  EXPECT_EQ(invoke("IoGetInitialStack", {}), Parent + 0x2000);
  success(Model->validateExecutionReturn(Parent, 0));
  success(Model->retireStack(Parent, 0x2000));
}

TEST_F(DriverKernelModel, StackQueryUsesFullWidthBoundsAtAdmittedIRQL) {
  constexpr uint64_t Base = 0x100008000ULL, Size = 0x2000;
  success(Memory->map(Base, Size, Read | Write));
  success(Model->activateStack(Base, Size));
  Model->enterExecution(Base);
  for (uint8_t Level : {0, 1, 2, 15}) {
    EXPECT_EQ(invoke("KfRaiseIrql", {Level}), 0u);
    invoke("IoGetStackLimits", {Scratch, Scratch + 8});
    EXPECT_EQ(integer(Scratch), Base);
    EXPECT_EQ(integer(Scratch + 8), Base + Size);
    if (Level <= 1)
      EXPECT_EQ(invoke("IoGetInitialStack", {}), Base + Size);
    else
      EXPECT_NE(failure("IoGetInitialStack", {}).find("IRQL"),
                std::string::npos);
    invoke("KeLowerIrql", {0});
  }
  success(Model->validateExecutionReturn(Base, 0));
  success(Model->retireStack(Base, Size));
}

TEST_F(DriverKernelModel, StackQueryWritesHighBeforeInaccessibleLow) {
  constexpr uint64_t Base = Scratch + 0x8000;
  success(Model->activateStack(Base, 0x2000));
  Model->enterExecution(Base);
  success(Memory->writeInteger(Scratch + 1, 0x1122334455667788ULL, 8));
  success(Memory->protect(Scratch, profile::PageSize, Read));
  EXPECT_FALSE(
      failure("IoGetStackLimits", {Scratch + 1, Scratch + 0x2001}).empty());
  EXPECT_EQ(integer(Scratch + 0x2001), Base + 0x2000);
  EXPECT_EQ(integer(Scratch + 1), 0x1122334455667788ULL);
}

TEST_F(DriverKernelModel, StackQueryRejectsInaccessibleHighBeforeLowWrite) {
  constexpr uint64_t Base = Scratch + 0x8000;
  success(Model->activateStack(Base, 0x2000));
  Model->enterExecution(Base);
  success(Memory->writeInteger(Scratch + 1, 0x1122334455667788ULL, 8));
  success(Memory->protect(Scratch + 0x2000, profile::PageSize, Read));
  EXPECT_FALSE(
      failure("IoGetStackLimits", {Scratch + 1, Scratch + 0x2001}).empty());
  EXPECT_EQ(integer(Scratch + 1), 0x1122334455667788ULL);
}

TEST_F(DriverKernelModel, StackQueryRejectsOverlappingOutputs) {
  success(Model->activateStack(Scratch + 0x8000, 0x2000));
  Model->enterExecution(Scratch + 0x8000);
  success(Memory->writeInteger(Scratch, 0x1122334455667788ULL, 8));
  success(Memory->writeInteger(Scratch + 8, 0x8899aabbccddeeffULL, 8));
  for (unsigned Offset : {0, 1, 4, 7}) {
    EXPECT_FALSE(
        failure("IoGetStackLimits", {Scratch, Scratch + Offset}).empty());
    EXPECT_FALSE(
        failure("IoGetStackLimits", {Scratch + Offset, Scratch}).empty());
  }
  EXPECT_FALSE(failure("IoGetStackLimits", {UINT64_MAX, Scratch}).empty());
  EXPECT_EQ(integer(Scratch), 0x1122334455667788ULL);
  EXPECT_EQ(integer(Scratch + 8), 0x8899aabbccddeeffULL);
  EXPECT_FALSE(Memory->fault());
}

TEST_F(DriverKernelModel, StackQueryActivationPreservesExactLifetimeExtent) {
  constexpr uint64_t Base = Scratch + 0x8000, Size = 0x2000;
  success(Model->activateStack(Base, Size));
  success(Model->activateStack(Base, Size));
  Model->enterExecution(Base);
  auto Refused = [&](llvm::Error E) {
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  };
  Refused(Model->activateStack(Base, Size + 1));
  Refused(Model->activateStack(Base + 0x1000, Size));
  Refused(Model->activateStack(0, Size));
  Refused(Model->activateStack(UINT64_MAX - 0x100, Size));
  Refused(Model->activateStack(Scratch, 0));
  Refused(Model->retireStack(Base, Size + 1));
  EXPECT_EQ(invoke("IoGetInitialStack", {}), Base + Size);
  success(Model->retireStack(Base, Size));
  Refused(Model->activateStack(Base, Size + 1));
  EXPECT_FALSE(failure("IoGetInitialStack", {}).empty());
  success(Model->activateStack(Base, Size));
  EXPECT_FALSE(failure("IoGetInitialStack", {}).empty());
  Model->enterExecution(Base);
  EXPECT_EQ(invoke("IoGetInitialStack", {}), Base + Size);
  success(Model->retireStack(Base, Size));
}

TEST_F(DriverNativeKernelModel,
       ProcessLookupSharesIdentityAndBalancesReferences) {
  Model->enterExecution(profile::StackBase);
  const auto Borrowed = invoke("IoGetCurrentProcess", {});
  ASSERT_NE(Borrowed, 0u);
  EXPECT_EQ(invoke("PsGetProcessId", {Borrowed}), 4u);
  EXPECT_NE(
      failure("ObfDereferenceObject", {Borrowed}).find("referenced process"),
      std::string::npos);
  for (unsigned I = 0; I < 2; ++I) {
    ASSERT_EQ(invoke("PsLookupProcessByProcessId", {4, Scratch}), 0u);
    EXPECT_EQ(integer(Scratch), Borrowed);
  }
  EXPECT_NE(denied(Borrowed, 8).find("opaque process"), std::string::npos);
  EXPECT_EQ(invoke("ObfDereferenceObject", {Borrowed}), 0u);
  EXPECT_EQ(invoke("ObfDereferenceObject", {Borrowed}), 0u);
  EXPECT_NE(
      failure("ObfDereferenceObject", {Borrowed}).find("referenced process"),
      std::string::npos);
  EXPECT_EQ(invoke("IoGetCurrentProcess", {}), Borrowed);
}

TEST_F(DriverNativeKernelModel,
       ThreadLookupSharesCurrentAndNestedThreadIdentity) {
  Model->enterExecution(profile::StackBase);
  // The initial guest CID is allocated independently of its stack/scheduler
  // key.
  ASSERT_EQ(
      invoke("PsLookupThreadByThreadId", {profile::InitialThreadID, Scratch}),
      0u);
  const uint64_t Object = integer(Scratch);
  const uint64_t ID = invoke("PsGetCurrentThreadId", {});
  EXPECT_NE(ID, profile::StackBase);
  EXPECT_EQ(invoke("PsGetCurrentThread", {}), Object);
  EXPECT_EQ(invoke("KeGetCurrentThread", {}), Object);
  EXPECT_EQ(invoke("PsGetThreadId", {Object}), ID);
  const auto Saved = Model->captureExecutionContext();
  Model->enterExecution(Scratch + 0x8000, profile::StackBase);
  EXPECT_EQ(invoke("PsGetCurrentThreadId", {}), ID);
  EXPECT_EQ(invoke("PsLookupThreadByThreadId", {ID, Scratch + 8}), 0u);
  EXPECT_EQ(integer(Scratch + 8), Object);
  success(Model->retireStack(Scratch + 0x8000, profile::PageSize));
  success(Model->restoreExecutionContext(Saved));
  EXPECT_NE(denied(Object, 8).find("opaque thread"), std::string::npos);
  invoke("ObfDereferenceObject", {Object});
  invoke("ObfDereferenceObject", {Object});
  EXPECT_NE(failure("ObfDereferenceObject", {Object}).find("referenced thread"),
            std::string::npos);
  EXPECT_EQ(invoke("PsGetCurrentThread", {}), Object);
}

TEST_F(DriverNativeKernelModel,
       Win32ThreadAssociationSharesIdentityAndClearsConditionally) {
  Model->enterExecution(profile::StackBase);
  const auto Thread = invoke("PsGetCurrentThread", {});
  EXPECT_EQ(invoke("PsGetCurrentThreadWin32Thread", {}), 0u);
  EXPECT_EQ(invoke("PsGetThreadWin32Thread", {Thread}), 0u);
  invoke("PsSetThreadWin32Thread", {Thread, Scratch, 0xdead});
  EXPECT_EQ(invoke("PsGetCurrentThreadWin32Thread", {}), Scratch);
  invoke("PsSetThreadWin32Thread", {Thread, 0, Scratch + 8});
  EXPECT_EQ(invoke("PsGetThreadWin32Thread", {Thread}), Scratch);
  invoke("PsSetThreadWin32Thread", {Thread, Scratch + 8, 0});
  EXPECT_EQ(invoke("PsGetThreadWin32Thread", {Thread}), Scratch + 8);
  invoke("PsSetThreadWin32Thread", {Thread, 0, Scratch + 8});
  EXPECT_EQ(invoke("PsGetCurrentThreadWin32Thread", {}), 0u);
  EXPECT_NE(failure("PsGetThreadWin32Thread", {0}).find("known thread"),
            std::string::npos);
  EXPECT_NE(
      failure("PsSetThreadWin32Thread", {Scratch, 1, 0}).find("known thread"),
      std::string::npos);
}

TEST_F(DriverNativeKernelModel,
       Win32ThreadBorrowedStateIsUnknownAndSurvivesNestedExecution) {
  Model->enterExecution(profile::StackBase);
  const auto System = invoke("PsGetCurrentThread", {});
  success(Model->setUserRequestContext(true, 100));
  const auto User = invoke("PsGetCurrentThread", {});
  EXPECT_NE(failure("PsGetCurrentThreadWin32Thread", {}).find("unspecified"),
            std::string::npos);
  EXPECT_NE(failure("PsSetThreadWin32Thread", {User, 0, 0}).find("unspecified"),
            std::string::npos);
  invoke("PsSetThreadWin32Thread", {User, Scratch, 0});
  const auto Saved = Model->captureExecutionContext();
  success(Model->inheritExecutionContext(Scratch + 0x8000, profile::StackBase));
  Model->enterExecution(Scratch + 0x8000, profile::StackBase);
  EXPECT_EQ(invoke("PsGetCurrentThreadWin32Thread", {}), Scratch);
  invoke("PsSetThreadWin32Thread", {User, Scratch + 8, 0});
  success(Model->retireStack(Scratch + 0x8000, profile::PageSize));
  success(Model->restoreExecutionContext(Saved));
  EXPECT_EQ(invoke("PsGetCurrentThreadWin32Thread", {}), Scratch + 8);
  success(Model->setUserRequestContext(true, 200));
  EXPECT_NE(failure("PsGetCurrentThreadWin32Thread", {}).find("unspecified"),
            std::string::npos);
  EXPECT_EQ(invoke("PsGetThreadWin32Thread", {User}), Scratch + 8);
  EXPECT_EQ(invoke("PsGetThreadWin32Thread", {System}), 0u);
}

TEST_F(DriverNativeKernelModel, Win32ThreadQueryRequiresActiveExecution) {
  EXPECT_NE(
      failure("PsGetCurrentThreadWin32Thread", {}).find("active execution"),
      std::string::npos);
}

TEST_F(DriverNativeKernelModel,
       Win32ThreadNewSystemThreadHasIndependentAssociation) {
  success(Memory->map(Entry, profile::PageSize, Read | Execute));
  Model->enterExecution(profile::StackBase);
  const auto Parent = invoke("PsGetCurrentThread", {});
  invoke("PsSetThreadWin32Thread", {Parent, Scratch, 0});
  ASSERT_EQ(
      invoke("PsCreateSystemThread", {Scratch + 0x100, 0, 0, 0, 0, Entry, 0}),
      0u);
  auto Next = llvm::cantFail(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  Model->enterExecution(profile::CallbackStackBase, Next->ID);
  const auto Child = invoke("PsGetCurrentThread", {});
  ASSERT_NE(Child, Parent);
  EXPECT_EQ(invoke("PsGetCurrentThreadWin32Thread", {}), 0u);
  invoke("PsSetThreadWin32Thread", {Child, Scratch + 16, 0});
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(invoke("PsGetCurrentThreadWin32Thread", {}), Scratch);
  EXPECT_EQ(invoke("PsGetThreadWin32Thread", {Child}), Scratch + 16);
}

TEST_F(DriverNativeKernelModel,
       ThreadProcessIdentitySurvivesForegroundContextSwitches) {
  Model->enterExecution(profile::StackBase);
  const auto System = invoke("PsGetCurrentThread", {});
  const auto SystemID = invoke("PsGetCurrentThreadId", {});
  const auto SystemProcess = invoke("IoGetCurrentProcess", {});
  EXPECT_EQ(invoke("PsGetThreadProcessId", {System}), 4u);
  EXPECT_EQ(invoke("PsGetThreadProcess", {System}), SystemProcess);
  EXPECT_EQ(invoke("IoThreadToProcess", {System}), SystemProcess);
  invoke("KeSetPriorityThread", {System, 15});
  const auto SystemContext = Model->captureExecutionContext();
  EXPECT_EQ(Model->threadPriority(profile::StackBase), 15);
  success(Model->setUserRequestContext(true, 0x3456));
  const auto User = invoke("PsGetCurrentThread", {});
  const auto UserID = invoke("PsGetCurrentThreadId", {});
  ASSERT_NE(User, System);
  ASSERT_NE(UserID, SystemID);
  EXPECT_EQ(invoke("PsGetThreadProcessId", {User}), 0x3456u);
  EXPECT_EQ(invoke("PsGetThreadProcess", {User}),
            invoke("IoGetCurrentProcess", {}));
  EXPECT_EQ(invoke("PsGetThreadProcessId", {System}), 4u);
  EXPECT_EQ(Model->threadPriority(profile::StackBase),
            thread_priority::Default);
  EXPECT_EQ(Model->readyThread(profile::StackBase, 1).Priority,
            thread_priority::Default);
  invoke("KeSetPriorityThread", {User, 7});
  EXPECT_EQ(Model->threadPriority(profile::StackBase), 7);
  EXPECT_NE(failure("ZwQueryInformationThread",
                    {windows::CurrentThreadPseudoHandle, 9, Scratch, 8, 0})
                .find("start"),
            std::string::npos);
  const auto Saved = Model->captureExecutionContext();
  success(Model->inheritExecutionContext(Scratch + 0x8000, profile::StackBase));
  Model->enterExecution(Scratch + 0x8000, profile::StackBase);
  EXPECT_EQ(invoke("PsGetCurrentThread", {}), User);
  EXPECT_EQ(invoke("PsGetThreadProcessId", {User}), 0x3456u);
  success(Model->retireStack(Scratch + 0x8000, profile::PageSize));
  success(Model->restoreExecutionContext(Saved));
  success(Model->setUserRequestContext(true, 0x5678));
  const auto Peer = invoke("PsGetCurrentThread", {});
  ASSERT_NE(Peer, User);
  EXPECT_EQ(invoke("PsGetThreadProcessId", {Peer}), 0x5678u);
  EXPECT_EQ(Model->readyThread(profile::StackBase, 1, &Saved).Priority, 7);
  EXPECT_EQ(Model->readyThread(profile::StackBase, 1, &SystemContext).Priority,
            15);
  EXPECT_EQ(invoke("KeSetPriorityThread", {User, 9}), 7u);
  EXPECT_EQ(Model->readyThread(profile::StackBase, 1, &Saved).Priority, 9);
  invoke("KeSetPriorityThread", {User, 7});
  success(Model->setUserRequestContext(true, 0x3456));
  EXPECT_EQ(invoke("PsGetCurrentThread", {}), User);
  EXPECT_EQ(Model->threadPriority(profile::StackBase), 7);
  EXPECT_EQ(invoke("PsLookupThreadByThreadId", {SystemID, Scratch}), 0u);
  EXPECT_EQ(integer(Scratch), System);
  invoke("ObfDereferenceObject", {System});
  success(Model->setUserRequestContext(false));
  EXPECT_EQ(invoke("PsGetCurrentThread", {}), System);
  EXPECT_EQ(Model->threadPriority(profile::StackBase), 15);
  EXPECT_EQ(Model->readyThread(profile::StackBase, 1).Priority, 15);
}

TEST_F(DriverNativeKernelModel,
       SystemThreadClientIDMatchesLookupAndScheduledIdentity) {
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true, 0x3456));
  success(
      Memory->writeInteger(Scratch + 0x100, windows::ObjectAttributesSize, 4));
  success(
      Memory->writeInteger(Scratch + 0x118, windows::ObjectKernelHandle, 4));
  ASSERT_EQ(
      invoke("PsCreateSystemThread", {Scratch, 0x10000000, Scratch + 0x100, 0,
                                      Scratch + 0x200, Entry, 0x9876}),
      0u);
  const auto Handle = integer(Scratch), PID = integer(Scratch + 0x200),
             CID = integer(Scratch + 0x208);
  EXPECT_EQ(PID, windows::SystemProcessID);
  ASSERT_NE(CID, 0u);
  ASSERT_EQ(invoke("PsLookupThreadByThreadId", {CID, Scratch + 8}), 0u);
  const auto Object = integer(Scratch + 8);
  EXPECT_EQ(invoke("PsGetThreadId", {Object}), CID);
  EXPECT_EQ(invoke("PsGetThreadProcessId", {Object}), PID);
  ASSERT_EQ(
      invoke("ObReferenceObjectByHandle", {Handle, 0, 0, 0, Scratch + 16, 0}),
      0u);
  EXPECT_EQ(integer(Scratch + 16), Object);
  invoke("ObfDereferenceObject", {Object});
  invoke("ZwClose", {Handle});
  auto Next = Model->nextScheduled(false);
  ASSERT_TRUE(bool(Next)) << llvm::toString(Next.takeError());
  ASSERT_TRUE(*Next);
  EXPECT_EQ((**Next).Arguments, std::vector<uint64_t>{0x9876});
  Model->enterExecution(Scratch + 0x8000, (**Next).ID);
  EXPECT_EQ(invoke("PsGetCurrentThreadId", {}), CID);
  EXPECT_EQ(invoke("PsGetCurrentThread", {}), Object);
  EXPECT_EQ(invoke("PsGetCurrentProcessId", {}), PID);
  invoke("PsTerminateSystemThread", {0});
  ASSERT_TRUE(Model->takeThreadTermination());
  success(Model->finishScheduled((**Next).ID));
  invoke("ObfDereferenceObject", {Object});
}

TEST_F(DriverNativeKernelModel,
       SystemThreadClientIDPreflightPreservesHandleAndIdentity) {
  Model->enterExecution(profile::StackBase);
  const auto Root = invoke("PsGetCurrentThreadId", {});
  success(Memory->writeInteger(Scratch, 0xdeadbeef, 8));
  success(Memory->writeInteger(Scratch + 0xff8, 0x12345678, 8));
  success(Memory->protect(Scratch + 0x1000, profile::PageSize, Read));
  EXPECT_NE(failure("PsCreateSystemThread",
                    {Scratch, 0x10000000, 0, 0, Scratch + 0xff8, Entry, 0})
                .find("permissions"),
            std::string::npos);
  EXPECT_EQ(integer(Scratch), 0xdeadbeefu);
  EXPECT_EQ(integer(Scratch + 0xff8), 0x12345678u);
  EXPECT_NE(failure("PsCreateSystemThread",
                    {Scratch, 0x10000000, 0, 0, Scratch + 0xfff8, Entry, 0})
                .find("permissions"),
            std::string::npos);
  EXPECT_EQ(integer(Scratch), 0xdeadbeefu);
  EXPECT_NE(failure("PsCreateSystemThread",
                    {Scratch, 0x10000000, 0, 0, Scratch + 4, Entry, 0})
                .find("overlapping"),
            std::string::npos);
  auto Empty = Model->nextScheduled(false);
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_FALSE(*Empty);
  success(Memory->protect(Scratch + 0x1000, profile::PageSize, Read | Write));
  ASSERT_EQ(invoke("PsCreateSystemThread",
                   {Scratch, 0x10000000, 0, 0, Scratch + 0x200, Entry, 0}),
            0u);
  EXPECT_EQ(integer(Scratch), profile::SystemThreadHandleBase);
  EXPECT_EQ(integer(Scratch + 0x208), Root + profile::ThreadIDStride);
}

TEST_F(DriverNativeKernelModel,
       GenericThreadHandlesKeepOneObjectAndRejectPrivileges) {
  Model->enterExecution(profile::StackBase);
  const auto Object = invoke("PsGetCurrentThread", {});
  for (uint64_t Access : {0x80000000ULL, 0x40000000ULL, 0x20000000ULL,
                          0x10000000ULL, 0xf0000040ULL}) {
    ASSERT_EQ(invoke("ObOpenObjectByPointer",
                     {Object, windows::ObjectKernelHandle, 0, Access, 0,
                      windows::KernelMode, Scratch}),
              0u);
    const auto Handle = integer(Scratch);
    ASSERT_EQ(
        invoke("ObReferenceObjectByHandle", {Handle, 0, 0, 0, Scratch + 8, 0}),
        0u);
    EXPECT_EQ(integer(Scratch + 8), Object);
    invoke("ObfDereferenceObject", {Object});
    invoke("ZwClose", {Handle});
  }
  success(Memory->writeInteger(Scratch, 0xdeadbeef, 8));
  EXPECT_NE(failure("PsCreateSystemThread",
                    {Scratch, 0x11000000, 0, 0, Scratch + 0x200, Entry, 0})
                .find("access mask"),
            std::string::npos);
  EXPECT_EQ(integer(Scratch), 0xdeadbeefu);
}

TEST_F(DriverNativeKernelModel,
       SystemThreadModernProfileAlwaysReturnsKernelHandle) {
  Model.reset();
  auto Backend = UnicornBackend::create(4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend));
  Memory = std::move(*Backend);
  success(Memory->map(Scratch, 0x10000, Read | Write));
  Model = std::make_unique<KernelModel>(*Memory, Result);
  DriverImage Image;
  Image.Base = 0x180000000;
  Image.Entry = Entry;
  Image.Size = 0x3000;
  DriverOptions Options;
  Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  success(Model->initialize(Image, Options));
  Model->enterExecution(profile::StackBase);
  ASSERT_EQ(invoke("PsCreateSystemThread",
                   {Scratch, 0x10000000, 0, 0, Scratch + 0x200, Entry, 0}),
            0u);
  const auto Handle = integer(Scratch);
  success(Model->setUserRequestContext(true, 100));
  ASSERT_EQ(
      invoke("ObReferenceObjectByHandle", {Handle, 0, 0, 0, Scratch + 8, 0}),
      0u);
  const auto Object = integer(Scratch + 8);
  EXPECT_EQ(invoke("PsGetThreadId", {Object}), integer(Scratch + 0x208));
  invoke("ObfDereferenceObject", {Object});
  EXPECT_EQ(invoke("ZwClose", {Handle}), 0u);
}

TEST_F(DriverNativeKernelModel,
       ThreadProcessQueryKeepsSystemCreationOwnerAfterExit) {
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true, 0x3456));
  ASSERT_EQ(invoke("PsCreateSystemThread", {Scratch, windows::ThreadAllAccess,
                                            Scratch + 0x100, 0, 0, Entry, 0}),
            windows::StatusInvalidParameter);
  success(
      Memory->writeInteger(Scratch + 0x100, windows::ObjectAttributesSize, 4));
  success(
      Memory->writeInteger(Scratch + 0x118, windows::ObjectKernelHandle, 4));
  ASSERT_EQ(invoke("PsCreateSystemThread", {Scratch, windows::ThreadAllAccess,
                                            Scratch + 0x100, 0, 0, Entry, 0}),
            0u);
  const auto Handle = integer(Scratch);
  ASSERT_EQ(
      invoke("ObReferenceObjectByHandle", {Handle, 0, 0, 0, Scratch + 8, 0}),
      0u);
  const auto Object = integer(Scratch + 8);
  EXPECT_EQ(invoke("PsGetThreadProcessId", {Object}), 4u);
  const auto Process = invoke("PsGetThreadProcess", {Object});
  EXPECT_EQ(invoke("PsGetProcessId", {Process}), 4u);
  auto Call = llvm::cantFail(Model->nextScheduled(false));
  ASSERT_TRUE(Call);
  Model->enterExecution(Call->ID);
  EXPECT_EQ(invoke("PsGetCurrentThread", {}), Object);
  EXPECT_EQ(invoke("PsGetThreadProcessId", {Object}), 4u);
  invoke("PsTerminateSystemThread", {0});
  ASSERT_TRUE(Model->takeThreadTermination());
  success(Model->finishScheduled(Call->ID));
  Model->enterExecution(profile::StackBase);
  invoke("ZwClose", {Handle});
  EXPECT_EQ(invoke("PsGetThreadProcessId", {Object}), 4u);
  EXPECT_EQ(invoke("IoThreadToProcess", {Object}), Process);
  invoke("ObfDereferenceObject", {Object});
  EXPECT_NE(failure("PsGetThreadProcessId", {Object}).find("known thread"),
            std::string::npos);
  EXPECT_NE(denied(Object, 1).find("freed"), std::string::npos);
}

TEST_F(DriverNativeKernelModel,
       FixedControlStateDoesNotInventOtherMSRProviders) {
  EXPECT_EQ(Model->guestCR4(), 0x620u);
  EXPECT_TRUE(Model->hasModelSpecificRegisters());
  auto CET = Model->readModelSpecificRegister(windows::IA32SupervisorCET);
  ASSERT_TRUE(bool(CET));
  EXPECT_EQ(*CET, 0u);
  for (uint32_t Index : {uint32_t(windows::IA32LStar), uint32_t(0x6a3)}) {
    auto Unknown = Model->readModelSpecificRegister(Index);
    ASSERT_FALSE(bool(Unknown));
    EXPECT_NE(llvm::toString(Unknown.takeError()).find("unmodeled guest MSR"),
              std::string::npos);
  }
}

TEST_F(DriverNativeKernelModel,
       ThreadProcessQueryRejectsUnknownIdentityAndIRQL) {
  Model->enterExecution(Scratch + 0x8000);
  const auto UnknownOwner = invoke("PsGetCurrentThread", {});
  for (const auto *Name :
       {"PsGetThreadProcessId", "PsGetThreadProcess", "IoThreadToProcess"}) {
    EXPECT_NE(failure(Name, {UnknownOwner}).find("creating process"),
              std::string::npos);
    EXPECT_NE(failure(Name, {Scratch}).find("known thread"), std::string::npos);
  }
  Model->enterExecution(profile::StackBase);
  const auto System = invoke("PsGetCurrentThread", {});
  const auto Old = invoke("KfRaiseIrql", {3});
  for (const auto *Name :
       {"PsGetThreadProcessId", "PsGetThreadProcess", "IoThreadToProcess"})
    EXPECT_NE(failure(Name, {System}).find("IRQL"), std::string::npos);
  invoke("KeLowerIrql", {Old});
}

TEST_F(DriverNativeKernelModel,
       ObjectTypeDataIsReadOnlyAndChecksThreadReferences) {
  using object_type::Kind;
  const uint64_t ThreadType = integer(object_type::slot(Kind::Thread));
  const uint64_t FileType = integer(object_type::slot(Kind::File));
  ASSERT_NE(ThreadType, 0u);
  EXPECT_NE(ThreadType, FileType);
  success(
      Model->validateGuestAccess(object_type::slot(Kind::Thread), 8, false));
  EXPECT_NE(
      denied(object_type::slot(Kind::Thread), 8, true).find("object-type"),
      std::string::npos);
  EXPECT_NE(denied(object_type::slot(Kind::Thread) + 7, 2).find("object-type"),
            std::string::npos);
  EXPECT_NE(denied(ThreadType, 1).find("object-type"), std::string::npos);
  ASSERT_EQ(invoke("PsCreateSystemThread",
                   {Scratch, windows::ThreadAllAccess, 0, 0, 0, Entry, 0}),
            0u);
  const uint64_t Handle = integer(Scratch);
  success(Memory->writeInteger(Scratch + 8, 0x12345678, 8));
  EXPECT_EQ(invoke("ObReferenceObjectByHandle",
                   {Handle, 0, FileType, 0, Scratch + 8, 0}),
            windows::StatusObjectTypeMismatch);
  EXPECT_EQ(integer(Scratch + 8), 0x12345678u);
  EXPECT_EQ(invoke("ObReferenceObjectByHandle",
                   {Handle, 0, ThreadType, 0, Scratch + 8, 0}),
            0u);
  const uint64_t Object = integer(Scratch + 8);
  invoke("ObfDereferenceObject", {Object});
  EXPECT_NE(failure("ObfDereferenceObject", {Object}).find("referenced thread"),
            std::string::npos);
  invoke("ZwClose", {Handle});
}

TEST_F(DriverNativeKernelModel,
       OpenPointerHandlesRetainBorrowedThreadsIndependently) {
  constexpr uint64_t Stack = Scratch + 0x8000;
  Model->enterExecution(Stack);
  const auto Object = invoke("PsGetCurrentThread", {});
  const auto Type = integer(object_type::slot(object_type::Kind::Thread));
  ASSERT_EQ(
      invoke("ObOpenObjectByPointer",
             {Object, windows::ObjectKernelHandle, 0, 0x40, Type, 0, Scratch}),
      0u);
  const auto First = integer(Scratch);
  ASSERT_EQ(
      invoke("ObOpenObjectByPointer",
             {Object, windows::ObjectKernelHandle, 0, 0, 0, 0, Scratch + 8}),
      0u);
  const auto Second = integer(Scratch + 8);
  ASSERT_NE(First, Second);
  EXPECT_NE(failure("ObfDereferenceObject", {Object}).find("referenced thread"),
            std::string::npos); // Handles did not create caller pointer refs.
  success(Model->retireStack(Stack, profile::PageSize));
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(invoke("ZwClose", {First}), 0u);
  EXPECT_EQ(invoke("ZwClose", {First}), windows::StatusInvalidHandle);
  EXPECT_EQ(invoke("ObReferenceObjectByHandle",
                   {Second, 0, Type, 0, Scratch + 16, 0}),
            0u);
  EXPECT_EQ(integer(Scratch + 16), Object);
  EXPECT_EQ(invoke("ZwClose", {Second}), 0u);
  EXPECT_NE(denied(Object, 1).find("opaque thread"), std::string::npos);
  invoke("ObfDereferenceObject", {Object});
  EXPECT_NE(denied(Object, 1).find("freed"), std::string::npos);
}

TEST_F(DriverNativeKernelModel,
       FailedOpenPointerDoesNotConsumeHandlesOrReferences) {
  Model->enterExecution(profile::StackBase);
  const auto Object = invoke("PsGetCurrentThread", {});
  const auto Type = integer(object_type::slot(object_type::Kind::Thread));
  const auto Wrong = integer(object_type::slot(object_type::Kind::File));
  success(Memory->writeInteger(Scratch, 0x12345678, 8));
  EXPECT_EQ(
      invoke("ObOpenObjectByPointer", {Object, 0, 0, 0, Wrong, 0, Scratch}),
      windows::StatusObjectTypeMismatch);
  EXPECT_EQ(integer(Scratch), 0u); // Native type failure clears PHANDLE.
  EXPECT_EQ(invoke("ObOpenObjectByPointer", {Object, 0, 0, 0, Type, 0, 0}),
            windows::StatusInvalidParameter);
  EXPECT_NE(
      failure("ObOpenObjectByPointer", {Scratch, 0, 0, 0, Type, 0, Scratch})
          .find("known live object"),
      std::string::npos);
  EXPECT_NE(failure("ObOpenObjectByPointer",
                    {Object, 0, 0, 0, Type, 0,
                     object_type::slot(object_type::Kind::Thread)})
                .find("object-type"),
            std::string::npos);
  ASSERT_EQ(
      invoke("ObOpenObjectByPointer", {Object, 0, 0, 0, Type, 0, Scratch}), 0u);
  EXPECT_EQ(integer(Scratch), profile::SystemThreadHandleBase);
  EXPECT_NE(failure("ObfDereferenceObject", {Object}).find("referenced thread"),
            std::string::npos);
  invoke("ZwClose", {integer(Scratch)});
}

TEST_F(DriverNativeKernelModel, OpenPointerWriteFaultLeavesNoUnloadOwnership) {
  Model->enterExecution(profile::StackBase);
  const auto Object = invoke("PsGetCurrentThread", {});
  success(Memory->writeInteger(
      Model->driverObject() + windows::DriverUnloadOffset, Entry, 8));
  success(Memory->protect(Scratch, profile::PageSize, Read));
  EXPECT_NE(failure("ObOpenObjectByPointer", {Object, 0, 0, 0, 0, 0, Scratch})
                .find("write"),
            std::string::npos);
  // A backend fault is terminal for mutation. Read-only unload validation can
  // still prove that the failed publication acquired neither kind of owner.
  EXPECT_NE(failure("ObfDereferenceObject", {Object}).find("referenced thread"),
            std::string::npos);
  auto Unload = Model->beginUnload();
  ASSERT_TRUE(bool(Unload)) << llvm::toString(Unload.takeError());
  success(Model->finishUnload());
}

TEST_F(DriverNativeKernelModel,
       ThreadStartQueryUsesInitialEntryAndTypedOutputWidths) {
  Model->enterExecution(profile::StackBase);
  success(Memory->writeInteger(Scratch + 8, 0x123456789abcdef0ULL, 8));
  success(Memory->writeInteger(Scratch + 16, 0xabcdef0112345678ULL, 8));
  EXPECT_EQ(invoke("ZwQueryInformationThread",
                   {windows::CurrentThreadPseudoHandle, 0x100000009ULL, Scratch,
                    0xaabbccdd00000008ULL, Scratch + 16}),
            0u);
  EXPECT_EQ(integer(Scratch), Entry);
  EXPECT_EQ(integer(Scratch + 8), 0x123456789abcdef0ULL);
  EXPECT_EQ(integer(Scratch + 16), 0xabcdef0100000008ULL);
  const auto Object = invoke("PsGetCurrentThread", {});
  EXPECT_NE(failure("ObfDereferenceObject", {Object}).find("referenced thread"),
            std::string::npos);
  const auto Saved = Model->captureExecutionContext();
  Model->enterExecution(Scratch + 0x8000, profile::StackBase);
  ASSERT_EQ(invoke("ZwQueryInformationThread",
                   {windows::CurrentThreadPseudoHandle, 9, Scratch, 8, 0}),
            0u);
  EXPECT_EQ(integer(Scratch), Entry);
  success(Model->retireStack(Scratch + 0x8000, profile::PageSize));
  success(Model->restoreExecutionContext(Saved));
}

TEST_F(DriverNativeKernelModel,
       ThreadStartQuerySurvivesSystemThreadExitUntilClose) {
  constexpr uint64_t Start = Entry + 0x100;
  ASSERT_EQ(invoke("PsCreateSystemThread",
                   {Scratch, windows::ThreadAllAccess, 0, 0, 0, Start, 0}),
            0u);
  const auto Handle = integer(Scratch);
  ASSERT_EQ(
      invoke("ObReferenceObjectByHandle", {Handle, 0, 0, 0, Scratch + 8, 0}),
      0u);
  const auto Object = integer(Scratch + 8);
  invoke("ObfDereferenceObject", {Object});
  ASSERT_EQ(invoke("ZwQueryInformationThread", {Handle, 9, Scratch + 16, 8, 0}),
            0u);
  EXPECT_EQ(integer(Scratch + 16), Start);
  auto Next = llvm::cantFail(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  Model->enterExecution(Scratch + 0x8000, Next->ID);
  ASSERT_EQ(invoke("ZwQueryInformationThread",
                   {windows::CurrentThreadPseudoHandle, 9, Scratch + 16, 8, 0}),
            0u);
  EXPECT_EQ(integer(Scratch + 16), Start);
  invoke("PsTerminateSystemThread", {0x1234});
  ASSERT_TRUE(Model->takeThreadTermination());
  success(Model->finishScheduled(Next->ID));
  Model->enterExecution(profile::StackBase);
  ASSERT_EQ(invoke("ZwQueryInformationThread", {Handle, 9, Scratch + 16, 8, 0}),
            0u);
  EXPECT_EQ(integer(Scratch + 16), Start);
  invoke("ZwClose", {Handle});
  EXPECT_NE(denied(Object, 1).find("freed"), std::string::npos);
  EXPECT_EQ(invoke("ZwQueryInformationThread", {Handle, 9, Scratch + 16, 8, 0}),
            windows::StatusInvalidHandle);
}

TEST_F(DriverNativeKernelModel,
       ThreadStartQueryLengthFailurePreservesBothOutputs) {
  Model->enterExecution(profile::StackBase);
  success(Memory->writeInteger(Scratch, 0x123456789abcdef0ULL, 8));
  success(Memory->writeInteger(Scratch + 8, 0x5555666677778888ULL, 8));
  for (uint64_t Length : {0ULL, 4ULL, 7ULL, 9ULL, 16ULL, 0xffffffffULL}) {
    EXPECT_EQ(invoke("ZwQueryInformationThread",
                     {0x1234, 9, Scratch, Length, Scratch + 8}),
              windows::StatusInfoLengthMismatch);
    EXPECT_EQ(integer(Scratch), 0x123456789abcdef0ULL);
    EXPECT_EQ(integer(Scratch + 8), 0x5555666677778888ULL);
  }
}

TEST_F(DriverNativeKernelModel, ThreadStartQueryReadsScheduledWorkItemEntry) {
  Model->enterExecution(profile::StackBase);
  ASSERT_EQ(invoke("IoCreateDevice",
                   {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch}),
            0u);
  const auto Device = integer(Scratch);
  const auto Work = invoke("IoAllocateWorkItem", {Device});
  constexpr uint64_t Start = Entry + 0x200;
  invoke("IoQueueWorkItem", {Work, Start, profile::DelayedWorkQueue, 0});
  auto Next = llvm::cantFail(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  Model->enterExecution(Scratch + 0x8000, Next->ID);
  ASSERT_EQ(invoke("ZwQueryInformationThread",
                   {windows::CurrentThreadPseudoHandle, 9, Scratch, 8, 0}),
            0u);
  EXPECT_EQ(integer(Scratch), Start);
  const auto Object = invoke("PsGetCurrentThread", {});
  success(Model->finishScheduled(Next->ID));
  EXPECT_NE(denied(Object, 1).find("freed"), std::string::npos);
  Model->enterExecution(profile::StackBase);
  invoke("IoFreeWorkItem", {Work});
  invoke("IoDeleteDevice", {Device});
}

TEST_F(DriverNativeKernelModel,
       ThreadStartQueryPreflightsAndOrdersOverlappingOutputs) {
  Model->enterExecution(profile::StackBase);
  constexpr uint64_t Length = Scratch + profile::PageSize;
  success(Memory->writeInteger(Scratch, 0x123456789abcdef0ULL, 8));
  success(Memory->protect(Length, profile::PageSize, Read));
  EXPECT_NE(failure("ZwQueryInformationThread",
                    {windows::CurrentThreadPseudoHandle, 9, Scratch, 8, Length})
                .find("writable"),
            std::string::npos);
  EXPECT_EQ(integer(Scratch), 0x123456789abcdef0ULL);
  success(Memory->protect(Length, profile::PageSize, Read | Write));
  ASSERT_EQ(
      invoke("ZwQueryInformationThread",
             {windows::CurrentThreadPseudoHandle, 9, Scratch, 8, Scratch}),
      0u);
  EXPECT_EQ(integer(Scratch), (Entry & 0xffffffff00000000ULL) | 8);
  ASSERT_EQ(
      invoke("ZwQueryInformationThread",
             {windows::CurrentThreadPseudoHandle, 9, Scratch, 8, Scratch + 4}),
      0u);
  EXPECT_EQ(integer(Scratch), (8ULL << 32) | (Entry & 0xffffffffULL));
}

TEST_F(DriverNativeKernelModel,
       ThreadStartQueryRejectsMissingFactsAndUnknownClasses) {
  Model->enterExecution(Scratch + 0x8000);
  EXPECT_NE(failure("ZwQueryInformationThread",
                    {windows::CurrentThreadPseudoHandle, 9, Scratch, 8, 0})
                .find("no modeled execution start"),
            std::string::npos);
  Model->enterExecution(profile::StackBase);
  for (uint64_t Class : {0ULL, 16ULL, 0xffffffffULL})
    EXPECT_NE(
        failure("ZwQueryInformationThread",
                {windows::CurrentThreadPseudoHandle, Class, Scratch, 8, 0})
            .find("unmodeled thread information class"),
        std::string::npos);
  EXPECT_EQ(invoke("ZwQueryInformationThread", {0xfffffffe, 9, Scratch, 8, 0}),
            windows::StatusInvalidHandle);
  invoke("KfRaiseIrql", {1});
  EXPECT_NE(failure("ZwQueryInformationThread",
                    {windows::CurrentThreadPseudoHandle, 9, Scratch, 8, 0})
                .find("IRQL"),
            std::string::npos);
  invoke("KeLowerIrql", {0});
}

TEST_F(DriverNativeKernelModel,
       OpenPointerHandlesEnforceProcessAndKernelOwnership) {
  Model->enterExecution(profile::StackBase);
  const auto Object = invoke("PsGetCurrentThread", {});
  ASSERT_EQ(invoke("ObOpenObjectByPointer", {Object, 0, 0, 0, 0, 0, Scratch}),
            0u);
  const auto Local = integer(Scratch);
  success(Model->setUserRequestContext(true, 0x1234));
  EXPECT_EQ(
      invoke("ObReferenceObjectByHandle", {Local, 0, 0, 0, Scratch + 8, 0}),
      windows::StatusInvalidHandle);
  EXPECT_EQ(invoke("ZwClose", {Local}), windows::StatusInvalidHandle);
  EXPECT_NE(
      failure("ObOpenObjectByPointer", {Object, 0, 0, 0, 0, 0, Scratch + 8})
          .find("OBJ_KERNEL_HANDLE"),
      std::string::npos);
  ASSERT_EQ(
      invoke("ObOpenObjectByPointer",
             {Object, windows::ObjectKernelHandle, 0, 0, 0, 0, Scratch + 8}),
      0u);
  const auto Kernel = integer(Scratch + 8);
  success(Model->setUserRequestContext(true, 0x5678));
  ASSERT_EQ(
      invoke("ObReferenceObjectByHandle", {Kernel, 0, 0, 0, Scratch + 16, 0}),
      0u);
  EXPECT_EQ(integer(Scratch + 16), Object);
  invoke("ObfDereferenceObject", {Object});
  invoke("ZwClose", {Kernel});
  success(Model->setUserRequestContext(false));
  EXPECT_EQ(invoke("ZwClose", {Local}), 0u);
}

TEST_F(DriverNativeKernelModel,
       OpenPointerRejectsUnmodeledChecksAndUnloadLeaks) {
  Model->enterExecution(profile::StackBase);
  const auto Object = invoke("PsGetCurrentThread", {});
  EXPECT_NE(
      failure("ObOpenObjectByPointer", {Object, 0, Scratch, 0, 0, 0, Scratch})
          .find("access state"),
      std::string::npos);
  EXPECT_EQ(invoke("ObOpenObjectByPointer", {Object, 0, 0, 0, 0, 1, Scratch}),
            windows::StatusObjectTypeMismatch);
  EXPECT_NE(failure("ObOpenObjectByPointer", {Object, 2, 0, 0, 0, 0, Scratch})
                .find("attributes"),
            std::string::npos);
  EXPECT_NE(failure("ObOpenObjectByPointer",
                    {Object, 0, 0, 0x01000000, 0, 0, Scratch})
                .find("access mask"),
            std::string::npos);
  invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("ObOpenObjectByPointer", {Object, 0, 0, 0, 0, 0, Scratch})
                .find("IRQL"),
            std::string::npos);
  invoke("KeLowerIrql", {0});
  invoke("KfRaiseIrql", {1});
  ASSERT_EQ(invoke("ObOpenObjectByPointer", {Object, 0, 0, 0, 0, 0, Scratch}),
            0u);
  const auto Handle = integer(Scratch);
  invoke("KeLowerIrql", {0});
  success(Memory->writeInteger(
      Model->driverObject() + windows::DriverUnloadOffset, Entry, 8));
  auto Unload = Model->beginUnload();
  ASSERT_TRUE(bool(Unload)) << llvm::toString(Unload.takeError());
  auto Leaked = Model->finishUnload();
  ASSERT_TRUE(bool(Leaked));
  EXPECT_NE(llvm::toString(std::move(Leaked)).find("thread handles"),
            std::string::npos);
  invoke("ZwClose", {Handle});
  success(Model->finishUnload());
}

TEST_F(DriverNativeKernelModel,
       ThreadLookupRejectsUnknownIDsWithoutWritingOutput) {
  Model->enterExecution(profile::StackBase);
  const auto ID = invoke("PsGetCurrentThreadId", {});
  success(Memory->writeInteger(Scratch, 0x12345678, 8));
  const uint64_t UnknownIDs[] = {0, 4, 0x100000000ULL + ID, UINT64_MAX};
  for (uint64_t Unknown : UnknownIDs) {
    EXPECT_EQ(invoke("PsLookupThreadByThreadId", {Unknown, Scratch}),
              windows::StatusInvalidCID);
    EXPECT_EQ(integer(Scratch), 0x12345678u);
  }
  const auto Object = invoke("PsGetCurrentThread", {});
  success(Memory->protect(Scratch, profile::PageSize, Read));
  EXPECT_NE(failure("PsLookupThreadByThreadId", {ID, Scratch}).find("write"),
            std::string::npos);
  EXPECT_EQ(invoke("PsLookupThreadByThreadId", {ID, 0}),
            windows::StatusInvalidParameter);
  EXPECT_NE(failure("ObfDereferenceObject", {Object}).find("referenced thread"),
            std::string::npos);
  EXPECT_NE(failure("PsGetThreadId", {Scratch}).find("known thread"),
            std::string::npos);
}

TEST_F(DriverNativeKernelModel,
       ThreadLookupReferenceRetainsExitedBorrowedObject) {
  constexpr uint64_t Stack = Scratch + 0x8000;
  Model->enterExecution(Stack);
  const auto Object = invoke("PsGetCurrentThread", {});
  const auto ID = invoke("PsGetThreadId", {Object});
  ASSERT_EQ(invoke("PsLookupThreadByThreadId", {ID, Scratch}), 0u);
  success(Model->retireStack(Stack, profile::PageSize));
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(invoke("PsLookupThreadByThreadId", {ID, Scratch}),
            windows::StatusInvalidCID);
  EXPECT_EQ(invoke("PsGetThreadId", {Object}), ID);
  EXPECT_NE(denied(Object, 8).find("opaque thread"), std::string::npos);
  invoke("ObfDereferenceObject", {Object});
  EXPECT_NE(denied(Object, 8).find("freed"), std::string::npos);
  EXPECT_NE(failure("PsGetThreadId", {Object}).find("known thread"),
            std::string::npos);
  success(Model->activateStack(Stack, profile::PageSize));
  Model->enterExecution(Stack);
  EXPECT_NE(invoke("PsGetCurrentThreadId", {}), ID);
  EXPECT_NE(invoke("PsGetCurrentThread", {}), Object);
}

TEST_F(DriverNativeKernelModel,
       ThreadLookupAndHandleReferencesShareSystemLifetime) {
  Model->enterExecution(profile::StackBase);
  const auto RootID = invoke("PsGetCurrentThreadId", {});
  ASSERT_EQ(invoke("PsCreateSystemThread",
                   {Scratch, windows::ThreadAllAccess, 0, 0, 0, Entry, 0}),
            0u);
  const auto Handle = integer(Scratch);
  ASSERT_EQ(invoke("ObReferenceObjectByHandle",
                   {Handle, 0, 0, windows::KernelMode, Scratch + 8, 0}),
            0u);
  const auto Object = integer(Scratch + 8);
  const auto ID = invoke("PsGetThreadId", {Object});
  EXPECT_NE(ID, RootID);
  EXPECT_NE(ID, Handle);
  ASSERT_EQ(invoke("PsLookupThreadByThreadId", {ID, Scratch + 16}), 0u);
  EXPECT_EQ(integer(Scratch + 16), Object);
  invoke("ObfDereferenceObject", {Object}); // Drop only the handle lookup ref.
  invoke("ZwClose", {Handle});
  auto Next = llvm::cantFail(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  Model->enterExecution(Scratch + 0x8000, Next->ID);
  EXPECT_EQ(invoke("PsGetCurrentThreadId", {}), ID);
  EXPECT_EQ(invoke("PsGetCurrentThread", {}), Object);
  invoke("PsTerminateSystemThread", {0});
  ASSERT_TRUE(Model->takeThreadTermination());
  success(Model->finishScheduled(Next->ID));
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(invoke("PsLookupThreadByThreadId", {ID, Scratch}),
            windows::StatusInvalidCID);
  EXPECT_EQ(invoke("PsGetThreadId", {Object}), ID);
  invoke("ObfDereferenceObject", {Object});
  EXPECT_NE(denied(Object, 8).find("freed"), std::string::npos);
}

TEST_F(DriverNativeKernelModel, ThreadLookupEnforcesIRQLAndUnloadReferences) {
  Model->enterExecution(profile::StackBase);
  const auto Object = invoke("PsGetCurrentThread", {});
  const auto ID = invoke("PsGetCurrentThreadId", {});
  invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("PsLookupThreadByThreadId", {ID, Scratch}).find("IRQL"),
            std::string::npos);
  EXPECT_EQ(invoke("PsGetThreadId", {Object}), ID);
  EXPECT_EQ(invoke("PsGetCurrentThreadId", {}), ID);
  invoke("KeLowerIrql", {0});
  ASSERT_EQ(invoke("PsLookupThreadByThreadId", {ID, Scratch}), 0u);
  success(Memory->writeInteger(
      Model->driverObject() + windows::DriverUnloadOffset, Entry, 8));
  auto Unload = Model->beginUnload();
  ASSERT_TRUE(bool(Unload)) << llvm::toString(Unload.takeError());
  auto Leaked = Model->finishUnload();
  ASSERT_TRUE(bool(Leaked));
  EXPECT_NE(llvm::toString(std::move(Leaked)).find("thread pointer references"),
            std::string::npos);
  invoke("ObfDereferenceObject", {Object});
  success(Model->finishUnload());
  EXPECT_TRUE(Result.UnloadCompleted);
}

TEST_F(DriverNativeKernelModel,
       ProcessLookupDoesNotTruncateIDsOrOverwriteFailedOutput) {
  success(Memory->writeInteger(Scratch, 0x12345678, 8));
  constexpr uint64_t UnknownIDs[] = {0, 12, 0x100000004ULL, UINT64_MAX};
  for (uint64_t ID : UnknownIDs) {
    EXPECT_EQ(invoke("PsLookupProcessByProcessId", {ID, Scratch}), 0xc000000bu);
    EXPECT_EQ(integer(Scratch), 0x12345678u);
  }
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true, 0x3456));
  EXPECT_EQ(invoke("PsLookupProcessByProcessId", {0x3456, Scratch}), 0u);
  const auto User = integer(Scratch);
  EXPECT_EQ(invoke("PsGetProcessId", {User}), 0x3456u);
  EXPECT_EQ(invoke("IoGetCurrentProcess", {}), User);
  EXPECT_EQ(invoke("ObfDereferenceObject", {User}), 0u);
}

TEST_F(DriverNativeKernelModel, FailedProcessLookupOutputAcquiresNoReference) {
  Model->enterExecution(profile::StackBase);
  const auto Borrowed = invoke("IoGetCurrentProcess", {});
  success(Memory->protect(Scratch, profile::PageSize, Read));
  EXPECT_NE(failure("PsLookupProcessByProcessId", {4, Scratch}).find("write"),
            std::string::npos);
  EXPECT_NE(
      failure("ObfDereferenceObject", {Borrowed}).find("referenced process"),
      std::string::npos);
}

TEST_F(DriverNativeKernelModel,
       ProcessLookupEnforcesIRQLAndUnloadReferenceOwnership) {
  Model->enterExecution(profile::StackBase);
  const auto Old = invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("PsLookupProcessByProcessId", {4, Scratch}).find("IRQL"),
            std::string::npos);
  success(Model->call("KeLowerIrql", {Old}).takeError());
  ASSERT_EQ(invoke("PsLookupProcessByProcessId", {4, Scratch}), 0u);
  const auto Object = integer(Scratch);
  success(Memory->writeInteger(
      Model->driverObject() + windows::DriverUnloadOffset, Entry, 8));
  auto Unload = Model->beginUnload();
  ASSERT_TRUE(bool(Unload)) << llvm::toString(Unload.takeError());
  auto Leaked = Model->finishUnload();
  ASSERT_TRUE(bool(Leaked));
  EXPECT_NE(
      llvm::toString(std::move(Leaked)).find("process pointer references"),
      std::string::npos);
  EXPECT_EQ(invoke("ObfDereferenceObject", {Object}), 0u);
  success(Model->finishUnload());
  EXPECT_TRUE(Result.UnloadCompleted);
}

TEST_F(DriverKernelModel,
       ExceptionUserAuthorityStaysBoundToThreadProcessAndLifetime) {
  constexpr uint32_t Process = DriverRequest::DefaultRequestorProcessID;
  constexpr uint64_t Child = Scratch + profile::PageSize;
  constexpr uint64_t Worker = Scratch + 2 * profile::PageSize;
  constexpr uint64_t WorkerChild = Scratch + 3 * profile::PageSize;
  const auto Allowed = [&] {
    return Model->canCatchUserAccess(profile::UserArenaBase, 1);
  };
  Model->enterExecution(profile::StackBase);
  success(Model->setUserRequestContext(true, Process));
  ASSERT_TRUE(Allowed());
  success(Model->inheritExecutionContext(Child, profile::StackBase));
  Model->enterExecution(Child, profile::StackBase);
  EXPECT_TRUE(Allowed());
  Model->enterExecution(Child, Worker);
  EXPECT_FALSE(Allowed());
  Model->enterExecution(Child, profile::StackBase);
  success(Model->setUserRequestContext(true, Process + 1));
  EXPECT_FALSE(Allowed());
  success(Model->setUserRequestContext(true, Process));
  EXPECT_TRUE(Allowed());
  success(Model->setUserRequestContext(false));
  EXPECT_FALSE(Allowed());
  success(Model->setUserRequestContext(true, Process));
  success(Model->retireStack(Child, profile::PageSize));
  Model->enterExecution(Child, profile::StackBase);
  EXPECT_FALSE(Allowed());

  // Even a lingering request process does not authorize a worker or its
  // exception callback. Only the active parent's existing authority transfers.
  Model->enterExecution(Worker);
  EXPECT_FALSE(Allowed());
  success(Model->inheritExecutionContext(WorkerChild, Worker));
  Model->enterExecution(WorkerChild, Worker);
  EXPECT_FALSE(Allowed());
}

TEST_F(DriverKernelModel, PoolOwnershipSurvivesBadTagAndRejectsUseAfterFree) {
  constexpr uint32_t Tag = 0x74736554;
  const uint64_t Pointer = invoke("ExAllocatePoolWithTag", {512, 48, Tag});
  ASSERT_NE(Pointer, 0u);
  EXPECT_EQ(Pointer % 16, 0u);
  success(Model->validateGuestAccess(Pointer, 48, true));
  EXPECT_NE(failure("ExFreePoolWithTag", {Pointer, Tag + 1}).find("tag"),
            std::string::npos);
  success(Model->validateGuestAccess(Pointer, 48, false));
  invoke("ExFreePoolWithTag", {Pointer, Tag});
  EXPECT_NE(denied(Pointer, 1).find("freed"), std::string::npos);
  EXPECT_NE(denied(Pointer + 47, 1, true).find("freed"), std::string::npos);
  EXPECT_NE(failure("ExFreePoolWithTag", {Pointer, Tag}).find("freed"),
            std::string::npos);
}

TEST_F(DriverKernelModel,
       SystemThreadObjectSurvivesHandleCloseUntilExitAndWait) {
  constexpr uint32_t ThreadAllAccess = 0x001fffff;
  constexpr uint64_t KernelModeWithRegisterHighBits =
      0xaabbccdd12345600 | windows::KernelMode;
  EXPECT_EQ(invoke("PsCreateSystemThread",
                   {Scratch, ThreadAllAccess, 0, 0, 0, Entry, Scratch + 0x100}),
            0u);
  const uint64_t Handle = integer(Scratch);
  ASSERT_NE(Handle, 0u);
  EXPECT_EQ(invoke("ObReferenceObjectByHandle",
                   {Handle, 0x100000, 0, KernelModeWithRegisterHighBits,
                    Scratch + 8, 0}),
            0u);
  const uint64_t Object = integer(Scratch + 8);
  ASSERT_NE(Object, 0u);
  EXPECT_NE(denied(Object, 1).find("opaque thread"), std::string::npos);
  EXPECT_EQ(invoke("ZwClose", {Handle}), 0u);
  EXPECT_EQ(invoke("ZwClose", {Handle}), 0xc0000008u);
  EXPECT_EQ(invoke("ObReferenceObjectByHandle",
                   {Handle, 0x100000, 0, 0, Scratch + 16, 0}),
            0xc0000008u);
  EXPECT_EQ(invoke("KeWaitForSingleObject",
                   {Object, 0, KernelModeWithRegisterHighBits, 0, 0}),
            0u);
  auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  EXPECT_EQ(Wait->Type, KernelModel::Wait::Kind::Thread);
  auto Invocation = Model->nextScheduled(false);
  ASSERT_TRUE(bool(Invocation)) << llvm::toString(Invocation.takeError());
  ASSERT_TRUE(*Invocation);
  EXPECT_EQ((**Invocation).Kind, KernelScheduler::CallbackKind::SystemThread);
  EXPECT_EQ((**Invocation).Object, Object);
  Model->enterExecution(Scratch + 0x8000, (**Invocation).ID);
  EXPECT_EQ(invoke("KeAreApcsDisabled", {}), 1u);
  EXPECT_EQ(invoke("KeAreAllApcsDisabled", {}), 0u);
  // The first completion attempt must not silently treat a normal return as
  // a terminated system thread.
  auto EarlyFinish = Model->finishScheduled((**Invocation).ID);
  ASSERT_TRUE(bool(EarlyFinish));
  EXPECT_NE(llvm::toString(std::move(EarlyFinish)).find("without PsTerminate"),
            std::string::npos);
  EXPECT_EQ(invoke("PsTerminateSystemThread", {0x1234}), 0u);
  ASSERT_EQ(Model->takeThreadTermination(), std::optional<uint32_t>(0x1234));
  success(Model->finishScheduled((**Invocation).ID));
  auto Status = Model->pollWait(*Wait);
  ASSERT_TRUE(bool(Status)) << llvm::toString(Status.takeError());
  ASSERT_TRUE(*Status);
  EXPECT_EQ(**Status, 0u);
  EXPECT_EQ(invoke("ObfDereferenceObject", {Object}), 0u);
  EXPECT_NE(denied(Object, 1).find("freed"), std::string::npos);
}

TEST_F(DriverKernelModel, SystemThreadClientIDNamesTheRunningThread) {
  constexpr uint32_t ThreadAllAccess = 0x001fffff;
  const uint64_t ClientID = Scratch + 0x20;
  success(Memory->write(ClientID, std::vector<uint8_t>(16, 0xff)));
  EXPECT_EQ(invoke("PsCreateSystemThread",
                   {Scratch, ThreadAllAccess, 0, 0, ClientID, Entry, 0}),
            0u);
  EXPECT_EQ(integer(ClientID), windows::SystemProcessID);
  const uint64_t ThreadID = integer(ClientID + 8);
  EXPECT_EQ(ThreadID % 4, 0u);
  const uint64_t Handle = integer(Scratch);
  EXPECT_EQ(invoke("ObReferenceObjectByHandle",
                   {Handle, 0x100000, 0, windows::KernelMode, Scratch + 8, 0}),
            0u);
  const uint64_t Object = integer(Scratch + 8);
  EXPECT_EQ(invoke("PsGetThreadId", {Object}), ThreadID);
  auto Invocation = Model->nextScheduled(false);
  ASSERT_TRUE(bool(Invocation)) << llvm::toString(Invocation.takeError());
  ASSERT_TRUE(*Invocation);
  Model->enterExecution(Scratch + 0x8000, (**Invocation).ID);
  EXPECT_EQ(invoke("PsGetCurrentThread", {}), Object);
  EXPECT_EQ(invoke("PsGetCurrentThreadId", {}), ThreadID);
  EXPECT_EQ(invoke("PsTerminateSystemThread", {0}), 0u);
  ASSERT_EQ(Model->takeThreadTermination(), std::optional<uint32_t>(0));
  success(Model->finishScheduled((**Invocation).ID));
  EXPECT_EQ(invoke("ZwClose", {Handle}), 0u);
  EXPECT_EQ(invoke("ObfDereferenceObject", {Object}), 0u);
  // A retired thread loses its ID, and a later thread never reuses it.
  EXPECT_NE(failure("PsGetThreadId", {Object}).find("known thread object"),
            std::string::npos);
  EXPECT_EQ(invoke("PsCreateSystemThread",
                   {Scratch, ThreadAllAccess, 0, 0, ClientID, Entry, 0}),
            0u);
  EXPECT_NE(integer(ClientID + 8), ThreadID);
  // Non-system process handles remain unsupported.
  EXPECT_NE(failure("PsCreateSystemThread",
                    {Scratch, ThreadAllAccess, 0, 1, 0, Entry, 0})
                .find("process handles"),
            std::string::npos);
}

TEST_F(DriverKernelModel, CriticalAndGuardedAPCRegionsPairOnOneThread) {
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(invoke("KeAreApcsDisabled", {}), 0u);
  EXPECT_EQ(invoke("KeAreAllApcsDisabled", {}), 0u);
  invoke("KeEnterCriticalRegion", {});
  invoke("KeEnterCriticalRegion", {});
  EXPECT_EQ(invoke("KeAreApcsDisabled", {}), 1u);
  EXPECT_EQ(invoke("KeAreAllApcsDisabled", {}), 0u);
  invoke("KeLeaveCriticalRegion", {});
  invoke("KeEnterGuardedRegion", {});
  EXPECT_EQ(invoke("KeAreApcsDisabled", {}), 1u);
  EXPECT_EQ(invoke("KeAreAllApcsDisabled", {}), 1u);
  auto Held = Model->validateExecutionReturn(profile::StackBase, 0);
  ASSERT_TRUE(bool(Held));
  EXPECT_NE(llvm::toString(std::move(Held)).find("APC-disable region"),
            std::string::npos);
  invoke("KeLeaveGuardedRegion", {});
  EXPECT_EQ(invoke("KeAreAllApcsDisabled", {}), 0u);
  invoke("KeLeaveCriticalRegion", {});
  EXPECT_EQ(invoke("KeAreApcsDisabled", {}), 0u);
  EXPECT_NE(failure("KeLeaveCriticalRegion", {}).find("no matching entry"),
            std::string::npos);
  EXPECT_NE(failure("KeLeaveGuardedRegion", {}).find("no matching entry"),
            std::string::npos);
  success(Model->validateExecutionReturn(profile::StackBase, 0));
}

TEST_F(DriverKernelModel, RecursiveMutexFollowsThreadAcrossNestedStacks) {
  using namespace mutex_test;
  Model->enterExecution(profile::StackBase);
  const uint64_t Mutex =
      invoke(kernel_api::ExAllocatePoolWithTag,
             {SehMutexPoolType, dispatcher::MutexSize, SehMutexPoolTag});
  ASSERT_NE(Mutex, 0u);
  invoke(kernel_api::KeInitializeMutex, {Mutex, 0});
  EXPECT_EQ(
      invoke(kernel_api::KeWaitForSingleObject,
             {Mutex, windows::ExecutiveWaitReason, windows::KernelMode, 0, 0}),
      windows::StatusSuccess);
  ASSERT_FALSE(Model->takeWait());
  const auto Parent = Model->captureExecutionContext();
  const uint64_t Child = Scratch + SehMutexChildOffset;
  Model->enterExecution(Child, Parent.Thread);
  EXPECT_EQ(invoke(kernel_api::KeAreApcsDisabled, {}), 1u);
  EXPECT_EQ(invoke(kernel_api::KeAreAllApcsDisabled, {}), 0u);
  EXPECT_EQ(
      invoke(kernel_api::KeWaitForSingleObject,
             {Mutex, windows::ExecutiveWaitReason, windows::KernelMode, 0, 0}),
      windows::StatusSuccess);
  EXPECT_FALSE(Model->takeWait());
  EXPECT_EQ(invoke(kernel_api::KeReadStateMutex, {Mutex}), UINT32_MAX);
  EXPECT_EQ(invoke(kernel_api::KeReleaseMutex, {Mutex, 0}), UINT32_MAX);
  EXPECT_EQ(invoke(kernel_api::KeAreApcsDisabled, {}), 1u);
  success(Model->validateExecutionReturn(Child, 0, true));
  success(Model->retireStack(Child, profile::PageSize));
  success(Model->restoreExecutionContext(Parent));
  EXPECT_EQ(invoke(kernel_api::KeReleaseMutex, {Mutex, 0}), 0u);
  EXPECT_EQ(invoke(kernel_api::KeAreApcsDisabled, {}), 0u);
  success(Model->validateExecutionReturn(profile::StackBase, 0));
  invoke(kernel_api::ExFreePoolWithTag, {Mutex, SehMutexPoolTag});
}

TEST_F(DriverKernelModel,
       NestedMutexOwnershipSurvivesAcquiringStackRetirement) {
  using namespace mutex_test;
  Model->enterExecution(profile::StackBase);
  const uint64_t Mutex =
      invoke(kernel_api::ExAllocatePoolWithTag,
             {SehMutexPoolType, dispatcher::MutexSize, SehMutexPoolTag});
  ASSERT_NE(Mutex, 0u);
  invoke(kernel_api::KeInitializeMutex, {Mutex, 0});
  const auto Parent = Model->captureExecutionContext();
  const uint64_t Child = Scratch + SehMutexChildOffset;
  Model->enterExecution(Child, Parent.Thread);
  invoke(kernel_api::KeWaitForSingleObject,
         {Mutex, windows::ExecutiveWaitReason, windows::KernelMode, 0, 0});
  ASSERT_FALSE(Model->takeWait());
  success(Model->validateExecutionReturn(Child, 0, true));
  success(Model->retireStack(Child, profile::PageSize));
  success(Model->restoreExecutionContext(Parent));
  EXPECT_EQ(invoke(kernel_api::KeAreApcsDisabled, {}), 1u);
  auto Held = Model->validateExecutionReturn(profile::StackBase, 0);
  ASSERT_TRUE(bool(Held));
  EXPECT_NE(llvm::toString(std::move(Held)).find(dispatcher::OwnedMutexReturn),
            std::string::npos);
  EXPECT_EQ(invoke(kernel_api::KeReleaseMutex, {Mutex, 0}), 0u);
  EXPECT_EQ(invoke(kernel_api::KeAreApcsDisabled, {}), 0u);
  success(Model->validateExecutionReturn(profile::StackBase, 0));
  invoke(kernel_api::ExFreePoolWithTag, {Mutex, SehMutexPoolTag});
}

TEST_F(DriverKernelModel, BlockedMutexAcquiresForWaitingThreadDuringPeerRun) {
  using namespace mutex_test;
  Model->enterExecution(profile::StackBase);
  const uint64_t Mutex =
      invoke(kernel_api::ExAllocatePoolWithTag,
             {SehMutexPoolType, dispatcher::MutexSize, SehMutexPoolTag});
  ASSERT_NE(Mutex, 0u);
  invoke(kernel_api::KeInitializeMutex, {Mutex, 0});
  invoke(kernel_api::KeWaitForSingleObject,
         {Mutex, windows::ExecutiveWaitReason, windows::KernelMode, 0, 0});
  ASSERT_FALSE(Model->takeWait());
  const uint64_t Peer = Scratch + SehMutexPeerOffset;
  const uint64_t Child = Scratch + SehMutexChildOffset;
  Model->enterExecution(Child, Peer);
  invoke(kernel_api::KeWaitForSingleObject,
         {Mutex, windows::ExecutiveWaitReason, windows::KernelMode, 0, 0});
  auto Pending = Model->takeWait();
  ASSERT_TRUE(Pending);
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(invoke(kernel_api::KeReleaseMutex, {Mutex, 0}), 0u);
  Model->enterExecution(Scratch + SehMutexOtherOffset);
  auto Acquired = Model->pollWait(*Pending);
  ASSERT_TRUE(bool(Acquired)) << llvm::toString(Acquired.takeError());
  ASSERT_EQ(*Acquired, std::optional<uint32_t>{windows::StatusSuccess});
  auto WrongOwner = Model->call(kernel_api::KeReleaseMutex.str(), {Mutex, 0});
  ASSERT_FALSE(bool(WrongOwner));
  uint32_t Code = 0;
  llvm::handleAllErrors(
      WrongOwner.takeError(),
      [&](const KernelGuestException &Error) { Code = Error.code(); },
      [&](const llvm::ErrorInfoBase &Error) {
        ADD_FAILURE() << Error.message();
      });
  EXPECT_EQ(Code, dispatcher::StatusMutantNotOwned);
  EXPECT_EQ(invoke(kernel_api::KeReadStateMutex, {Mutex}), 0u);
  // The wait was acquired for Peer, even though its nested stack is retired
  // before the owning thread resumes on its original stack.
  success(Model->retireStack(Child, profile::PageSize));
  Model->enterExecution(Peer);
  EXPECT_EQ(invoke(kernel_api::KeAreApcsDisabled, {}), 1u);
  EXPECT_EQ(invoke(kernel_api::KeReleaseMutex, {Mutex, 0}), 0u);
  EXPECT_EQ(invoke(kernel_api::KeAreApcsDisabled, {}), 0u);
  success(Model->validateExecutionReturn(Peer, 0));
  invoke(kernel_api::ExFreePoolWithTag, {Mutex, SehMutexPoolTag});
}

TEST_F(DriverKernelModel,
       ThreadPriorityUsesSignedABIAndCanonicalNestedIdentity) {
  Model->enterExecution(profile::StackBase);
  const uint64_t Object = llvm::cantFail(Model->currentThreadObject());
  EXPECT_EQ(invoke(kernel_api::KeQueryPriorityThread, {Object}),
            thread_priority::Default);
  EXPECT_EQ(invoke(kernel_api::KeSetPriorityThread,
                   {Object, 0xaabbccdd00000000ULL | thread_priority::Maximum}),
            thread_priority::Default);
  const auto Saved = Model->captureExecutionContext();
  Model->enterExecution(Scratch, profile::StackBase);
  EXPECT_EQ(llvm::cantFail(Model->currentThreadObject()), Object);
  EXPECT_EQ(invoke(kernel_api::KeSetPriorityThread,
                   {Object, thread_priority::Minimum}),
            thread_priority::Maximum);
  success(Model->retireStack(Scratch, profile::PageSize));
  success(Model->restoreExecutionContext(Saved));
  EXPECT_EQ(invoke(kernel_api::KeQueryPriorityThread, {Object}),
            thread_priority::Minimum);
  for (uint64_t Priority : {uint64_t(0), uint64_t(UINT32_MAX),
                            uint64_t(thread_priority::Maximum + 1)}) {
    EXPECT_EQ(failure(kernel_api::KeSetPriorityThread, {Object, Priority}),
              thread_priority::InvalidPriority);
    EXPECT_EQ(invoke(kernel_api::KeQueryPriorityThread, {Object}),
              thread_priority::Minimum);
  }
  EXPECT_EQ(failure(kernel_api::KeQueryPriorityThread, {Scratch}),
            thread_priority::UnknownThread);
  EXPECT_EQ(failure(kernel_api::KeSetPriorityThread,
                    {Scratch, thread_priority::Maximum}),
            thread_priority::UnknownThread);
  invoke(kernel_api::KfRaiseIrql, {windows::APCLevel});
  EXPECT_NE(failure(kernel_api::KeSetPriorityThread,
                    {Object, thread_priority::Maximum})
                .find(kernel_api::KeSetPriorityThread),
            std::string::npos);
  EXPECT_NE(failure(kernel_api::KeQueryPriorityThread, {Object})
                .find(kernel_api::KeQueryPriorityThread),
            std::string::npos);
  invoke(kernel_api::KeLowerIrql, {scheduler::PassiveLevel});
  EXPECT_EQ(invoke(kernel_api::KeQueryPriorityThread, {Object}),
            thread_priority::Minimum);
}

TEST_F(DriverKernelModel, DetachedStackReuseStartsWithFreshThreadClientID) {
  const uint64_t Base = Scratch + profile::PageSize;
  Model->enterExecution(Base);
  const uint64_t Object = llvm::cantFail(Model->currentThreadObject());
  const uint64_t ID = invoke(kernel_api::PsGetCurrentThreadId, {});
  EXPECT_EQ(invoke(kernel_api::PsGetThreadId, {Object}), ID);
  success(Model->retireStack(Base, profile::PageSize));
  EXPECT_NE(
      failure(kernel_api::PsGetThreadId, {Object}).find("known thread object"),
      std::string::npos);
  success(Model->activateStack(Base, profile::PageSize));
  Model->enterExecution(Base);
  const uint64_t Fresh = llvm::cantFail(Model->currentThreadObject());
  EXPECT_NE(Fresh, Object);
  const uint64_t FreshID = invoke(kernel_api::PsGetThreadId, {Fresh});
  EXPECT_NE(FreshID, ID);
  EXPECT_EQ(FreshID % 4, 0u);
  EXPECT_EQ(invoke(kernel_api::PsGetCurrentThreadId, {}), FreshID);
}

TEST_F(DriverKernelModel, DetachedStackReuseStartsWithFreshThreadPriority) {
  const uint64_t Base = Scratch + profile::PageSize;
  const uint64_t Child = Scratch + 2 * profile::PageSize;
  Model->enterExecution(Base);
  const uint64_t Object = llvm::cantFail(Model->currentThreadObject());
  invoke(kernel_api::KeSetPriorityThread, {Object, thread_priority::Maximum});
  Model->enterExecution(Child, Base);
  success(Model->retireStack(Child, profile::PageSize));
  EXPECT_EQ(invoke(kernel_api::KeQueryPriorityThread, {Object}),
            thread_priority::Maximum);
  Model->enterExecution(Base);
  success(Model->retireStack(Base, profile::PageSize));
  EXPECT_EQ(failure(kernel_api::KeQueryPriorityThread, {Object}),
            thread_priority::UnknownThread);
  success(Model->activateStack(Base, profile::PageSize));
  Model->enterExecution(Base);
  const uint64_t Fresh = llvm::cantFail(Model->currentThreadObject());
  EXPECT_NE(Fresh, Object);
  EXPECT_EQ(invoke(kernel_api::KeQueryPriorityThread, {Fresh}),
            thread_priority::Default);
  EXPECT_EQ(failure(kernel_api::KeSetPriorityThread,
                    {Object, thread_priority::Maximum}),
            thread_priority::UnknownThread);
}

TEST_F(DriverKernelModel,
       RetainedExitedThreadKeepsPriorityUntilObjectRetirement) {
  invoke(kernel_api::PsCreateSystemThread,
         {Scratch, windows::ThreadAllAccess, 0, 0, 0, Entry, 0});
  const uint64_t Handle = integer(Scratch);
  invoke(kernel_api::ObReferenceObjectByHandle,
         {Handle, 0, 0, windows::KernelMode, Scratch + 8, 0});
  const uint64_t Object = integer(Scratch + 8);
  invoke(kernel_api::KeSetPriorityThread, {Object, thread_priority::Maximum});
  auto Next = llvm::cantFail(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  Model->enterExecution(Scratch + 2 * profile::PageSize, Next->ID);
  EXPECT_EQ(invoke(kernel_api::KeQueryPriorityThread, {Object}),
            thread_priority::Maximum);
  invoke(kernel_api::PsTerminateSystemThread, {0});
  EXPECT_EQ(failure(kernel_api::KeSetPriorityThread,
                    {Object, thread_priority::Minimum}),
            thread_priority::ExitedThread);
  ASSERT_TRUE(Model->takeThreadTermination());
  success(Model->finishScheduled(Next->ID));
  EXPECT_EQ(invoke(kernel_api::KeQueryPriorityThread, {Object}),
            thread_priority::Maximum);
  invoke(kernel_api::ZwClose, {Handle});
  EXPECT_EQ(invoke(kernel_api::KeQueryPriorityThread, {Object}),
            thread_priority::Maximum);
  invoke(kernel_api::ObfDereferenceObject, {Object});
  EXPECT_EQ(failure(kernel_api::KeQueryPriorityThread, {Object}),
            thread_priority::UnknownThread);
}

TEST_F(DriverKernelModel, SpinLockFailuresPreserveOwnershipAndValidationOrder) {
  Model->enterExecution(profile::StackBase);
  constexpr uint32_t Tag = 0x4c4f434b;
  const uint64_t Lock = invoke("ExAllocatePoolWithTag", {0, 8, Tag});
  ASSERT_NE(Lock, 0u);
  invoke("KeInitializeSpinLock", {Lock});
  EXPECT_EQ(invoke("KeAcquireSpinLockRaiseToDpc", {Lock}), 0u);

  // Simulate corrupted storage independently of the model's write guard.
  // Release must check its variant and saved IRQL before inspecting the word.
  success(Memory->writeInteger(Lock, 2, 8));
  EXPECT_EQ(failure("KeReleaseSpinLockFromDpcLevel", {Lock}),
            "executive spin lock release variant does not match acquisition");
  EXPECT_EQ(failure("KeReleaseSpinLock", {Lock, 1}),
            "executive spin lock release must restore the saved IRQL");
  EXPECT_EQ(failure("KeReleaseSpinLock", {Lock, 0}),
            "executive spin lock storage was modified");
  EXPECT_EQ(Model->currentIRQL(), 2u);
  EXPECT_EQ(integer(Lock), 2u);
  EXPECT_EQ(invoke("KeTryToAcquireSpinLockAtDpcLevel", {Lock}), 0u);

  success(Memory->writeInteger(Lock, 1, 8));
  invoke("KeReleaseSpinLock", {Lock, 0xaabbccdd12345600});
  EXPECT_EQ(Model->currentIRQL(), 0u);
  EXPECT_EQ(integer(Lock), 0u);
  EXPECT_EQ(failure("KeReleaseSpinLock", {profile::UserArenaBase, 0}),
            "executive spin lock is not owned by this thread");
  EXPECT_EQ(failure("KeAcquireSpinLockAtDpcLevel", {profile::UserArenaBase}),
            "executive spin lock requires kernel storage");
  EXPECT_EQ(failure("KeAcquireSpinLockAtDpcLevel", {Lock}),
            "DPC-level spin-lock acquisition requires DISPATCH_LEVEL");
  success(Model->validateExecutionReturn(profile::StackBase, 0));
  invoke("ExFreePoolWithTag", {Lock, Tag});
}

TEST_F(DriverKernelModel, InterlockedListsLinkLikeTheKernel) {
  // ExInterlockedInsert{Head,Tail}List return the old neighbor or NULL, and
  // ExInterlockedRemoveHeadList leaves the removed entry's own links. The
  // lock is free again afterwards and IRQL never changes.
  Model->enterExecution(profile::StackBase);
  const uint64_t Block =
      invoke("ExAllocatePoolWithTag", {0, 0x100, 0x4c495354});
  ASSERT_NE(Block, 0u);
  const uint64_t Head = Block, E1 = Block + 0x10, E2 = Block + 0x20,
                 E3 = Block + 0x30, Lock = Block + 0x80;
  invoke("KeInitializeSpinLock", {Lock});
  success(Memory->writeInteger(Head, Head, 8));
  success(Memory->writeInteger(Head + 8, Head, 8));
  auto Link = [&](uint64_t Entry, unsigned Offset) {
    auto Value = Memory->readInteger(Entry + Offset, 8);
    return Value ? *Value : ~uint64_t(0);
  };

  EXPECT_EQ(invoke("ExInterlockedInsertTailList", {Head, E1, Lock}), 0u);
  EXPECT_EQ(invoke("ExInterlockedInsertTailList", {Head, E2, Lock}), E1);
  EXPECT_EQ(invoke("ExInterlockedInsertHeadList", {Head, E3, Lock}), E1);
  EXPECT_EQ(Link(Head, 0), E3);
  EXPECT_EQ(Link(Head, 8), E2);
  EXPECT_EQ(Link(E3, 0), E1);
  EXPECT_EQ(Link(E1, 8), E3);
  EXPECT_EQ(Model->currentIRQL(), 0u);
  EXPECT_EQ(Link(Lock, 0), 0u);

  EXPECT_EQ(invoke("ExInterlockedRemoveHeadList", {Head, Lock}), E3);
  EXPECT_EQ(Link(E3, 0), E1);
  EXPECT_EQ(Link(E3, 8), Head);
  EXPECT_EQ(Link(E1, 8), Head);
  const auto Previous = invoke("KfRaiseIrql", {2});
  EXPECT_EQ(invoke("ExInterlockedRemoveHeadList", {Head, Lock}), E1);
  EXPECT_EQ(invoke("ExInterlockedRemoveHeadList", {Head, Lock}), E2);
  EXPECT_EQ(invoke("ExInterlockedRemoveHeadList", {Head, Lock}), 0u);
  EXPECT_EQ(Model->currentIRQL(), 2u);
  invoke("KeLowerIrql", {Previous});
  EXPECT_EQ(Link(Head, 0), Head);
  EXPECT_EQ(Link(Head, 8), Head);
}

TEST_F(DriverKernelModel, InterlockedSingleListsAndCountersMatchTheKernel) {
  Model->enterExecution(profile::StackBase);
  const uint64_t Block =
      invoke("ExAllocatePoolWithTag", {0, 0x100, 0x4c495354});
  ASSERT_NE(Block, 0u);
  const uint64_t Head = Block, E1 = Block + 0x10, E2 = Block + 0x20,
                 Counter = Block + 0x40, Lock = Block + 0x80;
  invoke("KeInitializeSpinLock", {Lock});
  success(Memory->writeInteger(Head, 0, 8));
  EXPECT_EQ(invoke("ExInterlockedPushEntryList", {Head, E1, Lock}), 0u);
  EXPECT_EQ(invoke("ExInterlockedPushEntryList", {Head, E2, Lock}), E1);
  EXPECT_EQ(invoke("ExInterlockedPopEntryList", {Head, Lock}), E2);
  // The popped entry keeps its link to the old next entry.
  auto Next = Memory->readInteger(E2, 8);
  ASSERT_TRUE(static_cast<bool>(Next));
  EXPECT_EQ(*Next, E1);
  EXPECT_EQ(invoke("ExInterlockedPopEntryList", {Head, Lock}), E1);
  EXPECT_EQ(invoke("ExInterlockedPopEntryList", {Head, Lock}), 0u);

  // ExInterlockedAddUlong wraps at 32 bits and ignores the increment's upper
  // half; ExInterlockedAddLargeInteger wraps at 64 bits.
  success(Memory->writeInteger(Counter, 0x11111111ffffffffULL, 8));
  EXPECT_EQ(
      invoke("ExInterlockedAddUlong", {Counter, 0xabcdef0000000002ULL, Lock}),
      0xffffffffu);
  auto Value = Memory->readInteger(Counter, 8);
  ASSERT_TRUE(static_cast<bool>(Value));
  EXPECT_EQ(*Value, 0x1111111100000001ULL);
  success(Memory->writeInteger(Counter, ~uint64_t(0), 8));
  EXPECT_EQ(invoke("ExInterlockedAddLargeInteger", {Counter, 2, Lock}),
            ~uint64_t(0));
  Value = Memory->readInteger(Counter, 8);
  ASSERT_TRUE(static_cast<bool>(Value));
  EXPECT_EQ(*Value, 1u);
}

TEST_F(DriverKernelModel, InterlockedRoutinesRefuseWhatTheKernelCannotRun) {
  Model->enterExecution(profile::StackBase);
  const uint64_t Block =
      invoke("ExAllocatePoolWithTag", {0, 0x100, 0x4c495354});
  const uint64_t Paged = invoke("ExAllocatePoolWithTag", {1, 0x20, 0x4c495354});
  ASSERT_NE(Block, 0u);
  ASSERT_NE(Paged, 0u);
  const uint64_t Head = Block, Entry = Block + 0x10, Lock = Block + 0x80;
  invoke("KeInitializeSpinLock", {Lock});
  // A head whose first entry does not link back fails fast in the kernel.
  success(Memory->writeInteger(Head, Entry, 8));
  success(Memory->writeInteger(Head + 8, Entry, 8));
  success(Memory->writeInteger(Entry, Head, 8));
  success(Memory->writeInteger(Entry + 8, Entry, 8));
  for (const char *Name :
       {"ExInterlockedInsertHeadList", "ExInterlockedRemoveHeadList"}) {
    const std::string Error =
        std::string_view(Name) == "ExInterlockedRemoveHeadList"
            ? failure(Name, {Head, Lock})
            : failure(Name, {Head, Block + 0x20, Lock});
    EXPECT_NE(Error.find("FAST_FAIL_CORRUPT_LIST_ENTRY"), std::string::npos)
        << Name;
  }
  auto Untouched = Memory->readInteger(Head, 8);
  ASSERT_TRUE(static_cast<bool>(Untouched));
  EXPECT_EQ(*Untouched, Entry);

  success(Memory->writeInteger(Entry + 8, Head, 8));
  EXPECT_EQ(failure("ExInterlockedPushEntryList", {Paged, Entry, Lock}),
            "interlocked storage requires nonpaged memory");
  EXPECT_EQ(failure("ExInterlockedPopEntryList",
                    {profile::UserProbeLimit - 0x10, Lock}),
            "interlocked storage requires resident kernel memory");
  EXPECT_EQ(failure("ExInterlockedAddUlong", {Lock + 4, 1, Lock}),
            "interlocked storage overlaps its spin lock");
  EXPECT_EQ(invoke("KeAcquireSpinLockRaiseToDpc", {Lock}), 0u);
  EXPECT_EQ(failure("ExInterlockedRemoveHeadList", {Head, Lock}),
            "executive spin lock would deadlock on this cooperative processor");
  invoke("KeReleaseSpinLock", {Lock, 0});
  success(Memory->writeInteger(Lock, 1, 8));
  EXPECT_EQ(failure("ExInterlockedAddLargeInteger", {Block + 0x40, 1, Lock}),
            "executive spin lock must be initialized and free");
}

TEST_F(DriverKernelModel, SequencedListsEncodeDepthAndSequenceLikeTheKernel) {
  // A push adds 0x10001 to the first quadword, a pop decrements only the
  // 16-bit Depth and a flush clears it; the entry word's low four bits are
  // reserved and masked on every read.
  Model->enterExecution(profile::StackBase);
  const uint64_t Block =
      invoke("ExAllocatePoolWithTag", {0, 0x100, 0x534c5354});
  ASSERT_NE(Block, 0u);
  const uint64_t Header = Block, E1 = Block + 0x10, E2 = Block + 0x20;
  auto Word = [&](uint64_t Address) {
    auto Value = Memory->readInteger(Address, 8);
    return Value ? *Value : ~uint64_t(0);
  };
  success(Memory->writeInteger(Header, ~uint64_t(0), 8));
  invoke("InitializeSListHead", {Header});
  EXPECT_EQ(Word(Header), 0u);
  EXPECT_EQ(Word(Header + 8), 0u);
  EXPECT_EQ(invoke("ExpInterlockedPushEntrySList", {Header, E1}), 0u);
  EXPECT_EQ(invoke("ExpInterlockedPushEntrySList", {Header, E2}), E1);
  EXPECT_EQ(Word(Header), 0x20002u);
  EXPECT_EQ(Word(E2), E1);
  EXPECT_EQ(invoke("ExQueryDepthSList", {Header}), 2u);
  EXPECT_EQ(invoke("FirstEntrySList", {Header}), E2);
  EXPECT_EQ(invoke("ExpInterlockedPopEntrySList", {Header}), E2);
  EXPECT_EQ(Word(Header), 0x20001u);
  EXPECT_EQ(Word(E2), E1);
  EXPECT_EQ(invoke("ExpInterlockedFlushSList", {Header}), E1);
  EXPECT_EQ(Word(Header), 0x20000u);
  EXPECT_EQ(Word(Header + 8), 0u);
  EXPECT_EQ(invoke("ExpInterlockedPopEntrySList", {Header}), 0u);
  EXPECT_EQ(Word(Header), 0x20000u);

  // Depth wraps alone on a pop, which stores the popped link unchanged, but a
  // push carries it into Sequence.
  success(Memory->writeInteger(Header, 0x30000, 8));
  success(Memory->writeInteger(Header + 8, E1 | 5, 8));
  success(Memory->writeInteger(E1, E2 | 3, 8));
  EXPECT_EQ(invoke("ExpInterlockedPopEntrySList", {Header}), E1);
  EXPECT_EQ(Word(Header), 0x3ffffu);
  EXPECT_EQ(Word(Header + 8), E2 | 3);
  EXPECT_EQ(invoke("FirstEntrySList", {Header}), E2);
  EXPECT_EQ(invoke("ExpInterlockedPushEntrySList", {Header, E1}), E2);
  EXPECT_EQ(Word(Header), 0x50000u);
  EXPECT_EQ(Word(E1), E2);
}

TEST_F(DriverKernelModel, SequencedListsRaiseAndRefuseLikeTheKernel) {
  Model->enterExecution(profile::StackBase);
  const uint64_t Block =
      invoke("ExAllocatePoolWithTag", {0, 0x100, 0x534c5354});
  const uint64_t Paged = invoke("ExAllocatePoolWithTag", {1, 0x20, 0x534c5354});
  ASSERT_NE(Block, 0u);
  ASSERT_NE(Paged, 0u);
  const uint64_t Header = Block, Entry = Block + 0x10;
  invoke("InitializeSListHead", {Header});
  // InitializeSListHead raises like the kernel; cmpxchg16b would fault, and
  // a misaligned entry would lose its reserved low bits.
  EXPECT_EQ(failure("InitializeSListHead", {Header + 8}),
            "raised guest exception 0x80000002");
  EXPECT_EQ(failure("ExpInterlockedPushEntrySList", {Header + 8, Entry}),
            "sequenced list header requires 16-byte alignment");
  EXPECT_EQ(failure("ExpInterlockedPushEntrySList", {Header, Entry + 8}),
            "sequenced list entries require 16-byte alignment");
  EXPECT_EQ(failure("ExpInterlockedPushEntrySList", {Header, Header}),
            "sequenced list entry overlaps its header");
  auto Untouched = Memory->readInteger(Header, 8);
  ASSERT_TRUE(static_cast<bool>(Untouched));
  EXPECT_EQ(*Untouched, 0u);

  // Paged lookaside entries make paged lists valid below DISPATCH_LEVEL.
  invoke("InitializeSListHead", {Paged});
  EXPECT_EQ(invoke("ExpInterlockedPushEntrySList", {Paged, Paged + 0x10}), 0u);
  const auto Previous = invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("ExpInterlockedPopEntrySList", {Paged}).find("paged pool"),
            std::string::npos);
  EXPECT_EQ(invoke("ExpInterlockedPushEntrySList", {Header, Entry}), 0u);
  EXPECT_EQ(invoke("ExQueryDepthSList", {Header}), 1u);
  invoke("KeLowerIrql", {Previous});
}

TEST_F(DriverKernelModel, MDLMappingIgnoresUnspecifiedNarrowArgumentHighBits) {
  using namespace windows;
  invoke("IoCreateDevice",
         {Model->driverObject(), 0, 0, UnknownDeviceType, 0, 0, Scratch});
  for (unsigned Major : {0u, 14u}) {
    const uint64_t Slot =
        Model->driverObject() + DriverDispatchOffset + Major * 8;
    success(Model->validateGuestAccess(Slot, 8, true));
    success(Memory->writeInteger(Slot, Entry, 8));
  }
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  auto Call = Model->beginRequest(Create);
  ASSERT_TRUE(static_cast<bool>(Call)) << llvm::toString(Call.takeError());
  const uint64_t IRP = Call->Argument1;
  success(Model->validateGuestAccess(IRP + IRPStatusOffset, 16, true));
  success(Memory->writeInteger(IRP + IRPStatusOffset, 0, 4));
  success(Memory->writeInteger(IRP + IRPInformationOffset, 0, 8));
  invoke("IofCompleteRequest", {IRP, 0});
  success(Model->recordDispatchReturn(IRP, 0));
  success(Model->finalizeRequest(IRP));
  DriverRequest IO;
  IO.ControlCode = 0x222002;
  IO.OutputSize = 2;
  IO.DirectInput = {0xab, 0xcd};
  Call = Model->beginRequest(IO);
  ASSERT_TRUE(static_cast<bool>(Call)) << llvm::toString(Call.takeError());
  const uint64_t MDL = integer(Call->Argument1 + IRPMdlOffset);
  constexpr uint64_t Upper = 0xaabbccdd00000000;
  const uint64_t Pointer = invoke("MmMapLockedPagesSpecifyCache",
                                  {MDL, Upper, Upper | MmCached, 0,
                                   Upper | 0x100, Upper | NormalPagePriority});
  ASSERT_NE(Pointer, 0u);
  EXPECT_EQ(bytes(Pointer, 2), (std::vector<uint8_t>{0xab, 0xcd}));
  EXPECT_EQ(
      invoke("MmGetSystemAddressForMdlSafe", {MDL, Upper | NormalPagePriority}),
      Pointer);
}

TEST_F(DriverKernelModel, UnicodeInitializationUsesByteLengthsAndNullSource) {
  success(Memory->write(Scratch, std::vector<uint8_t>(16, 0xaa)));
  invoke("RtlInitUnicodeString", {Scratch, 0});
  EXPECT_EQ(integer(Scratch, 2), 0u);
  EXPECT_EQ(integer(Scratch + 2, 2), 0u);
  EXPECT_EQ(integer(Scratch + 8), 0u);
  EXPECT_EQ(integer(Scratch + 4, 4), 0xaaaaaaaau); // ABI padding is untouched.

  const uint64_t Source = Scratch + 0x100;
  utf16(Source, u"A\U0001f600"); // One ASCII unit and a surrogate pair.
  invoke("RtlInitUnicodeString", {Scratch, Source});
  EXPECT_EQ(integer(Scratch, 2), 6u);
  EXPECT_EQ(integer(Scratch + 2, 2), 8u);
  EXPECT_EQ(integer(Scratch + 8), Source);
}

TEST_F(DriverKernelModel, DevicesLinkCollideAndDeleteWithoutLosingLiveState) {
  const uint64_t NameA = Scratch + 0x100;
  const uint64_t NameB = Scratch + 0x200;
  const uint64_t AliasA = Scratch + 0x300;
  unicode(NameA, u"\\Device\\First");
  unicode(NameB, u"\\Device\\Second");
  unicode(AliasA, u"\\device\\FIRST");
  auto Create = [&](uint64_t Name) {
    return invoke("IoCreateDevice",
                  {Model->driverObject(), 32, Name, 0x22, 0x100, 0, Scratch});
  };
  ASSERT_EQ(Create(NameA), 0u);
  const uint64_t First = integer(Scratch);
  ASSERT_EQ(Create(NameB), 0u);
  const uint64_t Second = integer(Scratch);
  EXPECT_NE(First, Second);
  EXPECT_EQ(Create(AliasA), 0xc0000035u); // STATUS_OBJECT_NAME_COLLISION.
  EXPECT_EQ(integer(Model->driverObject() + 8), Second);
  EXPECT_EQ(integer(Second + 0x10), First);
  const auto Extension = bytes(integer(Second + 0x40), 32);
  EXPECT_TRUE(std::all_of(Extension.begin(), Extension.end(),
                          [](uint8_t Byte) { return Byte == 0; }));
  success(Model->snapshot());
  ASSERT_EQ(Result.Devices.size(), 2u);
  EXPECT_EQ(Result.Devices[0].Name, "\\Device\\Second");
  EXPECT_EQ(Result.Devices[1].Address, First);

  invoke("IoDeleteDevice", {First});
  EXPECT_EQ(integer(Second + 0x10), 0u);
  success(Model->snapshot());
  ASSERT_EQ(Result.Devices.size(), 1u);
  EXPECT_EQ(Result.Devices[0].Address, Second);
  EXPECT_NE(denied(First, 1).find("freed"), std::string::npos);
  invoke("IoDeleteDevice", {Second});
  EXPECT_EQ(integer(Model->driverObject() + 8), 0u);
  success(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
}

TEST_F(DriverKernelModel, DeviceTypeIsStoredAsGuestVisibleMetadata) {
  constexpr uint32_t VendorDeviceType = 0x8000;
  ASSERT_EQ(
      invoke("IoCreateDevice", {Model->driverObject(), 0, 0, VendorDeviceType,
                                windows::SecureOpen, 0, Scratch}),
      windows::StatusSuccess);
  const uint64_t Device = integer(Scratch);
  EXPECT_EQ(integer(Device + windows::DeviceTypeOffset, 4), VendorDeviceType);
  invoke("IoDeleteDevice", {Device});
}

TEST_F(DriverKernelModel, UnmappedDeviceOutputPreservesExistingListAndFault) {
  checkDeviceOutputFailure(0x50000000, BackendFaultKind::UnmappedMemory);
}

TEST_F(DriverKernelModel, ReadOnlyDeviceOutputPreservesExistingListAndFault) {
  const uint64_t Output = Scratch + 0x8000;
  success(Memory->writeInteger(Output, 0x12345678, 8));
  success(Memory->protect(Output, 0x1000, Read));
  checkDeviceOutputFailure(Output, BackendFaultKind::Protection);
  EXPECT_EQ(integer(Output), 0x12345678u);
}

TEST_F(DriverKernelModel,
       WorkItemKeepsDeletePendingDeviceUntilCallbackReturns) {
  invoke("IoCreateDevice", {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch});
  const auto Device = integer(Scratch);
  const auto Item = invoke("IoAllocateWorkItem", {Device});
  ASSERT_NE(Item, 0u);
  invoke("IoQueueWorkItem", {Item, Entry, profile::DelayedWorkQueue, Scratch});
  invoke("IoDeleteDevice", {Device});
  success(Model->snapshot());
  ASSERT_EQ(Result.Devices.size(), 1u);
  EXPECT_EQ(integer(Device + windows::DeviceReferenceCount, 4), 0u);
  auto Next = Model->nextScheduled(false);
  ASSERT_TRUE(bool(Next)) << llvm::toString(Next.takeError());
  ASSERT_TRUE(Next->has_value());
  EXPECT_EQ((**Next).Arguments, (std::vector<uint64_t>{Device, Scratch}));
  Model->enterExecution(Scratch + 2 * profile::PageSize, (**Next).ID);
  const auto Thread = llvm::cantFail(Model->currentThreadObject());
  invoke(kernel_api::KeSetPriorityThread, {Thread, thread_priority::Maximum});
  invoke("IoFreeWorkItem", {Item});
  success(Model->validateGuestAccess(Device, 2, false));
  success(Model->finishScheduled((**Next).ID));
  EXPECT_EQ(failure(kernel_api::KeQueryPriorityThread, {Thread}),
            thread_priority::UnknownThread);
  success(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
  EXPECT_NE(denied(Device, 1).find("freed"), std::string::npos);
}

TEST_F(DriverKernelModel, WorkItemAllocationExhaustionReturnsNull) {
  invoke("IoCreateDevice", {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch});
  const auto Device = integer(Scratch);
  ASSERT_NE(invoke("ExAllocatePoolWithTag",
                   {0, profile::KernelArenaSize - profile::PageSize, 0x1234}),
            0u);
  bool Exhausted = false;
  for (size_t I = 0; I < profile::PageSize / profile::WorkItemTokenSize; ++I) {
    auto Item = Model->call("IoAllocateWorkItem", {Device});
    ASSERT_TRUE(bool(Item)) << llvm::toString(Item.takeError());
    if (!*Item) {
      Exhausted = true;
      break;
    }
    invoke("IoFreeWorkItem", {*Item});
  }
  EXPECT_TRUE(Exhausted);
}

TEST_F(DriverKernelModel, SymbolicLinksShareTheSessionDosNamespace) {
  const uint64_t Name = Scratch + 0x100;
  const uint64_t Alias = Scratch + 0x200;
  const uint64_t Target = Scratch + 0x300;
  unicode(Name, u"\\DosDevices\\Example");
  unicode(Alias, u"\\??\\EXAMPLE");
  unicode(Target, u"\\Device\\Example");
  EXPECT_EQ(invoke("IoCreateSymbolicLink", {Name, Target}), 0u);
  EXPECT_EQ(invoke("IoCreateSymbolicLink", {Alias, Target}), 0xc0000035u);
  EXPECT_EQ(invoke("IoDeleteSymbolicLink", {Alias}), 0u);
  EXPECT_EQ(invoke("IoDeleteSymbolicLink", {Name}), 0xc0000034u);
  EXPECT_EQ(invoke("IoCreateSymbolicLink", {Name, Target}), 0u);
}

TEST_F(DriverKernelModel,
       MemoryOperationsRespectOverlapAndComparisonContracts) {
  ascii(Scratch, "abcdef");
  EXPECT_EQ(invoke("memmove", {Scratch + 2, Scratch, 4}), Scratch + 2);
  EXPECT_EQ(bytes(Scratch, 6),
            (std::vector<uint8_t>{'a', 'b', 'a', 'b', 'c', 'd'}));
  EXPECT_NE(failure("memcpy", {Scratch + 1, Scratch, 4}).find("overlapping"),
            std::string::npos);
  ascii(Scratch + 0x100, "abazcd");
  const auto Comparison = static_cast<int32_t>(
      static_cast<uint32_t>(invoke("memcmp", {Scratch, Scratch + 0x100, 6})));
  EXPECT_LT(Comparison, 0);
  EXPECT_EQ(invoke("RtlCompareMemory", {Scratch, Scratch + 0x100, 6}), 3u);
  EXPECT_EQ(invoke("memcmp", {Scratch, Scratch, 6}), 0u);
  EXPECT_EQ(invoke("RtlCompareMemory", {Scratch, Scratch, 6}), 6u);
}

TEST_F(DriverKernelModel, DebugLiteralsAreCapturedButVariadicFormatsFail) {
  ascii(Scratch, "driver 100%%\n");
  EXPECT_EQ(invoke("DbgPrint", {Scratch}), 0u);
  EXPECT_EQ(invoke("DbgPrintEx", {77, 2, Scratch}), 0u);
  ASSERT_EQ(Result.Messages.size(), 2u);
  EXPECT_EQ(Result.Messages[0], "driver 100%\n");
  EXPECT_EQ(Result.Messages[1], Result.Messages[0]);
  ascii(Scratch, "count=%u");
  EXPECT_NE(failure("DbgPrint", {Scratch}).find("variadic"), std::string::npos);
  EXPECT_EQ(Result.Messages.size(), 2u);
}

TEST_F(DriverKernelModel, MemoryAPIsCannotBypassOpaqueObjectAccessChecks) {
  const uint64_t DriverSection = Model->driverObject() + 0x28;
  EXPECT_NE(failure("memcpy", {Scratch, Model->driverObject() + 0x10, 4})
                .find("unmodeled"),
            std::string::npos);
  EXPECT_NE(failure("RtlZeroMemory", {DriverSection, 8}).find("opaque"),
            std::string::npos);
  EXPECT_NE(failure("RtlInitUnicodeString", {DriverSection, 0}).find("opaque"),
            std::string::npos);
  // A thunk is an execution token, never an image of ntoskrnl code/data.
  success(Memory->map(0x72000000, 0x1000, Read | Write | Execute));
  success(Memory->writeInteger(0x72000000, 0xcccccccc, 4));
  EXPECT_NE(failure("memcpy", {Scratch, 0x72000000, 4}).find("thunks"),
            std::string::npos);
  EXPECT_NE(failure("RtlCopyMemory", {Scratch, 0x72000000, 4}).find("thunks"),
            std::string::npos);
}

TEST_F(DriverKernelModel,
       UnknownArenaAndDefaultDispatchAddressesStayUnobservable) {
  EXPECT_NE(denied(0x700ff000, 8).find("unallocated"), std::string::npos);
  const uint64_t Dispatch = Model->driverObject() + 0x70;
  EXPECT_NE(denied(Dispatch, 8).find("unmodeled"), std::string::npos);
  EXPECT_NE(failure("memcpy", {Scratch, Dispatch, 8}).find("unmodeled"),
            std::string::npos);
  // Mirror a CPU write notification before storing the guest callback.
  success(Model->validateGuestAccess(Dispatch, 8, true));
  success(Memory->writeInteger(Dispatch, Entry + 0x20, 8));
  success(Model->validateGuestAccess(Dispatch, 8, false));
  success(Model->snapshot());
  EXPECT_EQ(Result.MajorFunctions[0], Entry + 0x20);
  EXPECT_EQ(Result.MajorFunctions[1], 0u); // No guest registration observed.
}

TEST_F(DriverKernelModel, CRuntimeStringsReadLikeTheKernel) {
  Model->enterExecution(profile::StackBase);
  const uint64_t A = Scratch + 0x100, B = Scratch + 0x140, C = Scratch + 0x180,
                 D = Scratch + 0x1c0, E = Scratch + 0x200, F = Scratch + 0x240,
                 G = Scratch + 0x280;
  const uint64_t WA = Scratch + 0x300, WB = Scratch + 0x340,
                 WC = Scratch + 0x380, WH = Scratch + 0x3c0,
                 WL = Scratch + 0x400;
  ascii(A, "hello world");
  ascii(B, "world");
  ascii(C, "abc");
  ascii(D, "abd");
  ascii(E, "ABC");
  ascii(F, "");
  ascii(G, "hel ");
  utf16(WA, u"hello");
  utf16(WB, u"HELLO");
  utf16(WC, u"ll");
  utf16(WH, u"hel");
  utf16(WL, u"l");

  // Lengths, bounded and unbounded; a satisfied count never reads further.
  EXPECT_EQ(invoke("strlen", {A}), 11u);
  EXPECT_EQ(invoke("wcslen", {WA}), 5u);
  EXPECT_EQ(invoke("strnlen", {A, 3}), 3u);
  EXPECT_EQ(invoke("strnlen", {A, 50}), 11u);
  EXPECT_EQ(invoke("wcsnlen", {WA, 10}), 5u);

  // Ordered compares normalize to -1/0/1; wcsncmp reports the raw difference.
  EXPECT_EQ(invoke("strcmp", {C, D}), uint64_t(uint32_t(-1)));
  EXPECT_EQ(invoke("strcmp", {D, C}), 1u);
  EXPECT_EQ(invoke("strcmp", {C, C}), 0u);
  EXPECT_EQ(invoke("strncmp", {C, D, 2}), 0u);
  EXPECT_EQ(invoke("strncmp", {C, D, 3}), uint64_t(uint32_t(-1)));
  EXPECT_EQ(invoke("wcscmp", {WA, WA}), 0u);
  EXPECT_EQ(invoke("wcsncmp", {WA, WB, 5}), 32u); // 'h' - 'H'

  // Case-insensitive compares fold ASCII A-Z before differencing.
  EXPECT_EQ(invoke("_stricmp", {C, E}), 0u);
  EXPECT_EQ(invoke("_wcsicmp", {WA, WB}), 0u);
  EXPECT_EQ(invoke("_strnicmp", {C, E, 2}), 0u);
  EXPECT_EQ(invoke("_wcsnicmp", {WA, WB, 5}), 0u);

  // Character search truncates the target and includes the terminator.
  EXPECT_EQ(invoke("strchr", {A, 'o'}), A + 4);
  EXPECT_EQ(invoke("strrchr", {A, 'o'}), A + 7);
  EXPECT_EQ(invoke("strchr", {A, 'z'}), 0u);
  EXPECT_EQ(invoke("strchr", {A, 0}), A + 11);
  EXPECT_EQ(invoke("strchr", {A, 0x100 | 'o'}), A + 4); // high bits ignored
  EXPECT_EQ(invoke("wcschr", {WA, 'l'}), WA + 4);
  EXPECT_EQ(invoke("wcsrchr", {WA, 'l'}), WA + 6);

  // Substring search; an empty needle returns the haystack.
  EXPECT_EQ(invoke("strstr", {A, B}), A + 6);
  EXPECT_EQ(invoke("strstr", {A, C}), 0u);
  EXPECT_EQ(invoke("strstr", {A, F}), A);
  EXPECT_EQ(invoke("wcsstr", {WA, WC}), WA + 4);

  // Spans of the initial run in and out of the control set.
  EXPECT_EQ(invoke("strspn", {A, G}), 4u);
  EXPECT_EQ(invoke("wcsspn", {WA, WH}), 4u);
  EXPECT_EQ(invoke("wcscspn", {WA, WL}), 2u);
}

TEST_F(DriverKernelModel, CRuntimeStringsFoldOnlyAsciiAndRefuseBadBuffers) {
  Model->enterExecution(profile::StackBase);
  const uint64_t P = Scratch + 0x100, Q = Scratch + 0x140;
  // Latin-1 letters that differ by 0x20 are not ASCII A-Z, so they never fold.
  success(Memory->write(P, std::vector<uint8_t>{0xc1, 0x00}));
  success(Memory->write(Q, std::vector<uint8_t>{0xe1, 0x00}));
  EXPECT_EQ(invoke("_stricmp", {P, Q}), uint64_t(uint32_t(-0x20)));

  // A null record is a clear refusal, not a synthesized fault.
  EXPECT_EQ(failure("strlen", {0}), "invalid or overflowing guest buffer");
  EXPECT_EQ(failure("wcscmp", {0, Q}), "invalid or overflowing guest buffer");
  // A zero count returns zero without dereferencing either pointer.
  EXPECT_EQ(invoke("strnlen", {0, 0}), 0u);
  EXPECT_EQ(invoke("strncmp", {0, 0, 0}), 0u);
  EXPECT_EQ(invoke("_wcsnicmp", {0, 0, 0}), 0u);
}

TEST_F(DriverKernelModel, CRuntimeStringWritesCopyPadTruncateAndAppend) {
  Model->enterExecution(profile::StackBase);
  const uint64_t Source = Scratch + 0x101, Dest = Scratch + 0x401;
  for (unsigned Width : {1, 2}) {
    SCOPED_TRACE(Width);
    const char *Copy = Width == 1 ? "strcpy" : "wcscpy";
    const char *NCopy = Width == 1 ? "strncpy" : "wcsncpy";
    const char *Cat = Width == 1 ? "strcat" : "wcscat";
    const char *NCat = Width == 1 ? "strncat" : "wcsncat";
    std::vector<uint8_t> Input;
    for (uint16_t Unit :
         {uint16_t('A'), uint16_t(0xe9), uint16_t(0x263a), uint16_t(0)}) {
      Input.push_back(uint8_t(Unit));
      if (Width == 2)
        Input.push_back(uint8_t(Unit >> 8));
    }
    success(Memory->write(Source, Input));
    auto Reset = [&]() {
      success(Memory->write(Dest, std::vector<uint8_t>(32, 0xcc)));
    };
    Reset();
    EXPECT_EQ(invoke(Copy, {Dest, Source}), Dest);
    auto Expected = Input;
    Expected.resize(32, 0xcc);
    EXPECT_EQ(bytes(Dest, 32), Expected);
    for (unsigned Count = 0; Count <= 6; ++Count) {
      SCOPED_TRACE(Count);
      Reset();
      EXPECT_EQ(invoke(NCopy, {Dest, Source, Count}), Dest);
      Expected = Input;
      Expected.resize(Count * Width, 0);
      Expected.resize(32, 0xcc);
      EXPECT_EQ(bytes(Dest, 32), Expected);
      Reset();
      success(Memory->writeInteger(Dest, 'P', Width));
      success(Memory->writeInteger(Dest + Width, 0, Width));
      EXPECT_EQ(invoke(NCat, {Dest, Source, Count}), Dest);
      Expected.assign(Width, 0);
      Expected[0] = 'P';
      Expected.insert(Expected.end(), Input.begin(),
                      Input.begin() + std::min(Count, 3u) * Width);
      Expected.resize(Expected.size() + Width, 0);
      Expected.resize(32, 0xcc);
      EXPECT_EQ(bytes(Dest, 32), Expected);
    }
    Reset();
    success(Memory->writeInteger(Dest, 'P', Width));
    success(Memory->writeInteger(Dest + Width, 0, Width));
    EXPECT_EQ(invoke(Cat, {Dest, Source}), Dest);
    EXPECT_EQ(bytes(Dest + Width, Input.size()), Input);
    Reset();
    success(Memory->writeInteger(Dest, 0, Width));
    EXPECT_EQ(invoke(NCat, {Dest, Source, 0x100000001}), Dest);
    EXPECT_EQ(bytes(Dest, Input.size()), Input); // size_t is not truncated.
  }
}

TEST_F(DriverKernelModel, CRuntimeStringWriteZeroCountsAndReadBoundaries) {
  Model->enterExecution(profile::StackBase);
  constexpr uint64_t Invalid = 0x90000000;
  for (const char *Name : {"strncpy", "wcsncpy", "strncat"})
    EXPECT_EQ(invoke(Name, {Invalid, Invalid, 0}), Invalid);
  utf16(Scratch, u"P");
  EXPECT_EQ(invoke("wcsncat", {Scratch, Invalid, 0}), Scratch);
  success(Memory->protect(Scratch, 0x1000, Read));
  EXPECT_NE(failure("wcsncat", {Scratch, Invalid, 0}).find("writable"),
            std::string::npos);
  success(Memory->protect(Scratch, 0x1000, Read | Write));
  for (unsigned Width : {1, 2}) {
    const uint64_t Source = Scratch + 0x10000 - Width;
    const uint64_t Dest = Scratch + 0x100;
    const char *NCopy = Width == 1 ? "strncpy" : "wcsncpy";
    const char *NCat = Width == 1 ? "strncat" : "wcsncat";
    success(Memory->writeInteger(Source, 'X', Width));
    EXPECT_EQ(invoke(NCopy, {Dest, Source, 1}), Dest);
    EXPECT_EQ(integer(Dest, Width), 'X');
    success(Memory->writeInteger(Dest, 0, Width));
    EXPECT_EQ(invoke(NCat, {Dest, Source, 1}), Dest);
    EXPECT_EQ(integer(Dest, Width), 'X');
    EXPECT_EQ(integer(Dest + Width, Width), 0u);
  }
  // A real read fault retires the backend; do not reuse it for later calls.
  EXPECT_FALSE(failure("wcsncat", {Invalid, Invalid, 0}).empty());
}

TEST_F(DriverKernelModel, CRuntimeStringCopyReadFaultPreservesDestination) {
  Model->enterExecution(profile::StackBase);
  const uint64_t Source = Scratch + 0xfffe, Dest = Scratch + 0x100;
  success(Memory->writeInteger(Source, 'X', 2));
  success(Memory->writeInteger(Dest, 0x1122334455667788, 8));
  EXPECT_FALSE(failure("wcsncpy", {Dest, Source, 2}).empty());
  EXPECT_EQ(integer(Dest), 0x1122334455667788u);
}

TEST_F(DriverKernelModel,
       CRuntimeStringWritesRejectOverlapFaultsAndHugeCounts) {
  Model->enterExecution(profile::StackBase);
  const uint64_t Source = Scratch + 0x100, Dest = Scratch + 0x200;
  ascii(Source, "abcdef");
  const auto Before = bytes(Source, 16);
  for (const char *Name : {"strcpy", "strcat"})
    EXPECT_NE(failure(Name, {Source, Source}).find("overlapping"),
              std::string::npos);
  EXPECT_NE(failure("strncpy", {Source + 1, Source, 4}).find("overlapping"),
            std::string::npos);
  EXPECT_EQ(bytes(Source, 16), Before);
  EXPECT_NE(failure("wcsncpy", {Dest, Source, UINT64_MAX}).find("overflows"),
            std::string::npos);
  EXPECT_NE(failure("strncpy", {Dest, Source, 0x100000001}).find("limit"),
            std::string::npos);
  EXPECT_FALSE(failure("strcpy", {UINT64_MAX, Source}).empty());
  const uint64_t Partial = Scratch + 0xffff;
  success(Memory->writeInteger(Partial, 0xcc, 1));
  EXPECT_FALSE(failure("strcpy", {Partial, Source}).empty());
  EXPECT_EQ(integer(Partial, 1), 0xccu); // Preflight refuses the whole write.
  const uint64_t Pool = invoke("ExAllocatePoolWithTag", {1, 32, 0x1234});
  ascii(Pool, "paged");
  invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("strcpy", {Dest, Pool}).find("paged pool"),
            std::string::npos);
  EXPECT_NE(failure("strcpy", {Pool, Source}).find("paged pool"),
            std::string::npos);
  invoke("KeLowerIrql", {0});
  invoke("ExFreePool", {Pool});
  EXPECT_NE(failure("strcpy", {Pool, Source}).find("freed"), std::string::npos);
}

TEST_F(DriverKernelModel, QueuedSpinLocksRaiseStoreAndRestoreLikeTheKernel) {
  Model->enterExecution(profile::StackBase);
  constexpr uint32_t Tag = 0x4b51534c; // 'KQSL'
  const uint64_t Lock = invoke("ExAllocatePoolWithTag", {0, 8, Tag});
  const uint64_t Handle = invoke("ExAllocatePoolWithTag", {0, 0x18, Tag});
  const uint64_t Other = invoke("ExAllocatePoolWithTag", {0, 0x18, Tag});
  ASSERT_NE(Lock, 0u);
  ASSERT_NE(Handle, 0u);
  ASSERT_NE(Other, 0u);
  invoke("KeInitializeSpinLock", {Lock});
  EXPECT_EQ(Model->currentIRQL(), 0u);
  // Acquire raises to DISPATCH_LEVEL, stores the handle in the lock, and stores
  // the lock and saved IRQL in the handle.
  EXPECT_EQ(invoke("KeAcquireInStackQueuedSpinLock", {Lock, Handle}), 0u);
  EXPECT_EQ(Model->currentIRQL(), 2u);
  EXPECT_EQ(integer(Lock), Handle);
  EXPECT_EQ(integer(Handle + windows::LockQueueLockOffset), Lock);
  EXPECT_EQ(integer(Handle + windows::LockQueueNextOffset), 0u);
  EXPECT_EQ(integer(Handle + windows::LockQueueOldIrqlOffset, 1), 0u);
  // A second acquisition of a held lock cannot make progress here.
  EXPECT_EQ(failure("KeAcquireInStackQueuedSpinLock", {Lock, Other}),
            "executive spin lock would deadlock on this cooperative processor");
  // Initializing a held lock would clobber the token, so it is refused.
  EXPECT_EQ(failure("KeInitializeSpinLock", {Lock}),
            "cannot initialize a held executive spin lock");
  // A corrupted saved IRQL in the handle is caught before the lock is cleared.
  success(Memory->writeInteger(Handle + windows::LockQueueOldIrqlOffset, 7, 1));
  EXPECT_EQ(failure("KeReleaseInStackQueuedSpinLock", {Handle}),
            "queued spin lock handle's saved IRQL was modified");
  success(Memory->writeInteger(Handle + windows::LockQueueOldIrqlOffset, 0, 1));
  // Release restores the saved IRQL and frees the lock.
  EXPECT_EQ(invoke("KeReleaseInStackQueuedSpinLock", {Handle}), 0u);
  EXPECT_EQ(integer(Lock), 0u);
  EXPECT_EQ(Model->currentIRQL(), 0u);
  invoke("ExFreePoolWithTag", {Lock, Tag});
  invoke("ExFreePoolWithTag", {Handle, Tag});
  invoke("ExFreePoolWithTag", {Other, Tag});
}

TEST_F(DriverKernelModel, QueuedSpinLocksAtDpcLevelAndRefuseMismatches) {
  Model->enterExecution(profile::StackBase);
  constexpr uint32_t Tag = 0x4b51534c;
  const uint64_t Lock = invoke("ExAllocatePoolWithTag", {0, 8, Tag});
  const uint64_t Handle = invoke("ExAllocatePoolWithTag", {0, 0x18, Tag});
  ASSERT_NE(Lock, 0u);
  ASSERT_NE(Handle, 0u);
  invoke("KeInitializeSpinLock", {Lock});
  // The AtDpcLevel form requires the caller already at DISPATCH_LEVEL.
  EXPECT_EQ(failure("KeAcquireInStackQueuedSpinLockAtDpcLevel", {Lock, Handle}),
            "DPC-level queued spin-lock acquisition requires DISPATCH_LEVEL");
  const auto Previous = invoke("KfRaiseIrql", {2});
  EXPECT_EQ(invoke("KeAcquireInStackQueuedSpinLockAtDpcLevel", {Lock, Handle}),
            0u);
  EXPECT_EQ(integer(Lock), Handle);
  // The raising release does not match a DPC-level acquisition.
  EXPECT_EQ(failure("KeReleaseInStackQueuedSpinLock", {Handle}),
            "queued spin lock release variant does not match acquisition");
  // The matching release leaves IRQL at DISPATCH_LEVEL.
  EXPECT_EQ(invoke("KeReleaseInStackQueuedSpinLockFromDpcLevel", {Handle}), 0u);
  EXPECT_EQ(integer(Lock), 0u);
  EXPECT_EQ(Model->currentIRQL(), 2u);
  invoke("KeLowerIrql", {Previous});

  // A contended lock stops clearly, and an unheld handle is not owned.
  success(Memory->writeInteger(Lock, 0x1234, 8));
  EXPECT_EQ(failure("KeAcquireInStackQueuedSpinLock", {Lock, Handle}),
            "executive spin lock must be initialized and free");
  success(Memory->writeInteger(Handle + windows::LockQueueLockOffset, 0, 8));
  EXPECT_EQ(failure("KeReleaseInStackQueuedSpinLock", {Handle}),
            "queued spin lock is not owned by this thread");
  invoke("ExFreePoolWithTag", {Lock, Tag});
  invoke("ExFreePoolWithTag", {Handle, Tag});
}

TEST_F(DriverKernelModel, RundownProtectionCountsReferencesLikeTheKernel) {
  Model->enterExecution(profile::StackBase);
  constexpr uint32_t Tag = 0x4e555252; // 'RRUN'
  const uint64_t Ref = invoke("ExAllocatePoolWithTag", {0, 8, Tag});
  ASSERT_NE(Ref, 0u);
  // Initialization clears the whole word regardless of prior contents.
  success(Memory->writeInteger(Ref, ~uint64_t(0), 8));
  invoke("ExInitializeRundownProtection", {Ref});
  EXPECT_EQ(integer(Ref), 0u);
  // Each acquisition adds two and returns TRUE; release subtracts two.
  EXPECT_EQ(invoke("ExAcquireRundownProtection", {Ref}), 1u);
  EXPECT_EQ(integer(Ref), 2u);
  EXPECT_EQ(invoke("ExAcquireRundownProtection", {Ref}), 1u);
  EXPECT_EQ(integer(Ref), 4u);
  EXPECT_EQ(invoke("ExReleaseRundownProtection", {Ref}), 0u);
  EXPECT_EQ(invoke("ExReleaseRundownProtection", {Ref}), 0u);
  EXPECT_EQ(integer(Ref), 0u);
  // With nothing outstanding the wait runs the object down in place.
  EXPECT_EQ(invoke("ExWaitForRundownProtectionRelease", {Ref}), 0u);
  EXPECT_EQ(integer(Ref), 1u);
  // Further acquisitions fail while run-down is in effect and write nothing.
  EXPECT_EQ(invoke("ExAcquireRundownProtection", {Ref}), 0u);
  EXPECT_EQ(integer(Ref), 1u);
  // A repeat wait on an already run-down object is a no-op.
  EXPECT_EQ(invoke("ExWaitForRundownProtectionRelease", {Ref}), 0u);
  EXPECT_EQ(integer(Ref), 1u);
  // Reinitialization makes it usable again.
  invoke("ExReInitializeRundownProtection", {Ref});
  EXPECT_EQ(integer(Ref), 0u);
  EXPECT_EQ(invoke("ExAcquireRundownProtection", {Ref}), 1u);
  EXPECT_EQ(invoke("ExReleaseRundownProtection", {Ref}), 0u);
  invoke("ExFreePoolWithTag", {Ref, Tag});
}

TEST_F(DriverKernelModel, RundownProtectionRefusesUnreachableCooperativePaths) {
  Model->enterExecution(profile::StackBase);
  constexpr uint32_t Tag = 0x4e555252;
  const uint64_t Ref = invoke("ExAllocatePoolWithTag", {0, 8, Tag});
  ASSERT_NE(Ref, 0u);
  invoke("ExInitializeRundownProtection", {Ref});
  // A release with nothing outstanding underflows, so it is refused and the
  // count is untouched.
  EXPECT_EQ(failure("ExReleaseRundownProtection", {Ref}),
            "run-down protection release without a matching acquisition");
  EXPECT_EQ(integer(Ref), 0u);
  // Waiting while a reference is still outstanding would block on an event.
  EXPECT_EQ(invoke("ExAcquireRundownProtection", {Ref}), 1u);
  EXPECT_EQ(failure("ExWaitForRundownProtectionRelease", {Ref}),
            "cannot wait for outstanding run-down references on this "
            "cooperative processor");
  EXPECT_EQ(integer(Ref), 2u);
  EXPECT_EQ(invoke("ExReleaseRundownProtection", {Ref}), 0u);
  // Releasing after run-down has completed follows the kernel's null wait-block
  // path and is refused.
  invoke("ExRundownCompleted", {Ref});
  EXPECT_EQ(integer(Ref), 1u);
  EXPECT_EQ(failure("ExReleaseRundownProtection", {Ref}),
            "cannot release run-down protection once run-down has started on "
            "this cooperative processor");
  EXPECT_EQ(integer(Ref), 1u);
  // A null record is a clear refusal.
  EXPECT_EQ(failure("ExAcquireRundownProtection", {0}),
            "run-down protection requires 8-byte aligned kernel storage");
  invoke("ExFreePoolWithTag", {Ref, Tag});
}

TEST_F(DriverKernelModel, RundownProtectionCountedFormsStepByTwiceTheCount) {
  Model->enterExecution(profile::StackBase);
  constexpr uint32_t Tag = 0x4e555252;
  const uint64_t Ref = invoke("ExAllocatePoolWithTag", {0, 8, Tag});
  ASSERT_NE(Ref, 0u);
  invoke("ExInitializeRundownProtection", {Ref});
  // A counted acquisition adds twice the count; a counted release removes it.
  EXPECT_EQ(invoke("ExAcquireRundownProtectionEx", {Ref, 3}), 1u);
  EXPECT_EQ(integer(Ref), 6u);
  EXPECT_EQ(invoke("ExReleaseRundownProtectionEx", {Ref, 2}), 0u);
  EXPECT_EQ(integer(Ref), 2u);
  // Releasing more than is outstanding underflows and is refused untouched.
  EXPECT_EQ(failure("ExReleaseRundownProtectionEx", {Ref, 2}),
            "run-down protection release without a matching acquisition");
  EXPECT_EQ(integer(Ref), 2u);
  // The ULONG doubles in 32 bits, so 0x80000000 steps by zero, and only the
  // low 32 bits of the argument register count.
  EXPECT_EQ(invoke("ExAcquireRundownProtectionEx", {Ref, 0x80000000}), 1u);
  EXPECT_EQ(integer(Ref), 2u);
  EXPECT_EQ(invoke("ExAcquireRundownProtectionEx", {Ref, 0xffffffff00000001}),
            1u);
  EXPECT_EQ(integer(Ref), 4u);
  EXPECT_EQ(invoke("ExReleaseRundownProtectionEx", {Ref, 2}), 0u);
  EXPECT_EQ(integer(Ref), 0u);
  // After run-down, a counted acquisition fails without writing.
  invoke("ExRundownCompleted", {Ref});
  EXPECT_EQ(invoke("ExAcquireRundownProtectionEx", {Ref, 5}), 0u);
  EXPECT_EQ(integer(Ref), 1u);
  invoke("ExFreePoolWithTag", {Ref, Tag});
}

TEST_F(DriverNativeKernelModel,
       LoadedModuleHasImageIdentityAndBoundedMutableFlags) {
  const uint64_t Module = integer(Model->driverObject() + 0x28);
  ASSERT_NE(Module, 0u);
  EXPECT_EQ(integer(Module), Module);
  EXPECT_EQ(integer(Module + 8), Module);
  EXPECT_EQ(integer(Module + 0x30), 0x180000000u);
  EXPECT_EQ(integer(Module + 0x38), Entry);
  EXPECT_EQ(integer(Module + 0x40, 4), 0x3000u);
  EXPECT_EQ(integer(Module + 0x68, 4), 0u);
  EXPECT_EQ(integer(Module + 0x6c, 2), 1u);
  const uint64_t Name = integer(Module + 0x60);
  const std::string Expected = "NeverDDriver.sys";
  ASSERT_EQ(integer(Module + 0x58, 2), Expected.size() * 2);
  for (size_t I = 0; I < Expected.size(); ++I)
    EXPECT_EQ(integer(Name + I * 2, 2), uint8_t(Expected[I]));
  success(Memory->writeInteger(Scratch, 0x02004000, 4));
  invoke("RtlCopyMemory", {Module + 0x68, Scratch, 4});
  EXPECT_EQ(integer(Module + 0x68, 4), 0x02004000u);
  EXPECT_NE(failure("RtlZeroMemory", {Module + 0x30, 8}).find("read-only"),
            std::string::npos);
  EXPECT_NE(failure("RtlZeroMemory", {Module + 0x68, 8}).find("read-only"),
            std::string::npos);
  EXPECT_EQ(integer(Module + 0x68, 4), 0x02004000u);
  EXPECT_NE(failure("memcpy", {Scratch, Module + 0x10, 8}).find("opaque"),
            std::string::npos);
  EXPECT_NE(failure("memcpy", {Scratch, Module + 0x70, 8}).find("opaque"),
            std::string::npos);
  EXPECT_NE(failure("RtlZeroMemory", {Name, 2}).find("read-only"),
            std::string::npos);
  EXPECT_EQ(integer(Name, 2), uint64_t('N'));
}

TEST_F(DriverNativeKernelModel,
       AddressValidityIsAPureResidencyQueryNotObjectAccess) {
  EXPECT_EQ(invoke("MmIsAddressValid", {Scratch}), 1u);
  EXPECT_EQ(invoke("MmIsAddressValid", {Scratch + 0xffff}), 1u);
  EXPECT_EQ(invoke("MmIsAddressValid", {Scratch + 0x10000}), 0u);
  EXPECT_EQ(invoke("MmIsAddressValid", {0}), 0u);
  EXPECT_EQ(invoke("MmIsAddressValid", {0x12345000}), 0u);
  EXPECT_EQ(invoke("MmIsAddressValid", {UINT64_MAX}), 0u);
  const uint64_t Module = integer(Model->driverObject() + 0x28);
  EXPECT_EQ(invoke("MmIsAddressValid", {Module}), 1u);
  const uint64_t OpaqueField = Model->driverObject() + 0x10;
  EXPECT_EQ(invoke("MmIsAddressValid", {OpaqueField}), 1u);
  EXPECT_NE(denied(OpaqueField, 4).find("unmodeled"), std::string::npos);
  EXPECT_EQ(integer(Module + 0x68, 4), 0u);
  EXPECT_FALSE(Memory->fault());
}

TEST_F(DriverNativeKernelModel,
       BugCheckRegistrationRetainsStorageUntilDeregistered) {
  success(Memory->map(Entry, 0x1000, Read | Execute));
  const uint64_t Record =
      invoke("ExAllocatePoolWithTag", {0, 0x30, 0x74736554});
  ASSERT_NE(Record, 0u);
  success(Memory->writeInteger(Record + 0x2c, 0, 1));
  success(Memory->write(Scratch, {'T', 'e', 's', 't', 0}));
  EXPECT_EQ(
      invoke("KeRegisterBugCheckReasonCallback", {Record, Entry, 2, Scratch}),
      1u);
  EXPECT_EQ(integer(Record + 0x10), Entry);
  EXPECT_EQ(integer(Record + 0x18), Scratch);
  EXPECT_EQ(integer(Record + 0x28, 4), 2u);
  EXPECT_EQ(integer(Record + 0x2c, 1), 1u);
  EXPECT_EQ(
      invoke("KeRegisterBugCheckReasonCallback", {Record, Entry, 2, Scratch}),
      0u);
  EXPECT_NE(failure("ExFreePool", {Record}).find("retained"),
            std::string::npos);
  EXPECT_NE(failure("RtlZeroMemory", {Record, 0x30}).find("opaque"),
            std::string::npos);
  EXPECT_EQ(invoke("KeDeregisterBugCheckReasonCallback", {Record}), 1u);
  EXPECT_EQ(integer(Record + 0x2c, 1), 0u);
  EXPECT_EQ(invoke("KeDeregisterBugCheckReasonCallback", {Record}), 0u);
  EXPECT_EQ(
      invoke("KeRegisterBugCheckReasonCallback", {Record, Entry, 7, Scratch}),
      1u);
  EXPECT_EQ(invoke("KeDeregisterBugCheckReasonCallback", {Record}), 1u);
  invoke("ExFreePool", {Record});
}

TEST_F(DriverNativeKernelModel,
       RejectedBugCheckRegistrationPreservesGuestBytes) {
  success(Memory->map(Entry, 0x1000, Read | Execute));
  success(Memory->write(Scratch + 0x100, {'T', 0}));
  const auto Arguments = [&](uint32_t Reason) {
    return invoke("KeRegisterBugCheckReasonCallback",
                  {Scratch, Entry, Reason, Scratch + 0x100});
  };
  EXPECT_EQ(Arguments(0), 0u);
  EXPECT_EQ(Arguments(8), 0u);
  EXPECT_EQ(integer(Scratch + 0x2c, 1), 0u);
  EXPECT_NE(failure("KeRegisterBugCheckReasonCallback",
                    {Scratch, Scratch + 0x200, 2, Scratch + 0x100})
                .find("executable"),
            std::string::npos);
  EXPECT_EQ(integer(Scratch + 0x2c, 1), 0u);
  success(Memory->writeInteger(Scratch + 0x2c, 2, 1));
  EXPECT_EQ(Arguments(2), 0u);
  EXPECT_EQ(integer(Scratch + 0x2c, 1), 2u);
  success(Memory->writeInteger(Scratch + 0x2c, 0, 1));
  EXPECT_EQ(Arguments(2), 1u);
  EXPECT_EQ(invoke("KeDeregisterBugCheckReasonCallback", {Scratch}), 1u);
}

TEST_F(DriverNativeKernelModel,
       BugCheckComponentBorrowDoesNotOwnItsPoolStorage) {
  success(Memory->map(Entry, 0x1000, Read | Execute));
  const auto Name = invoke("ExAllocatePoolWithTag", {0, 16, 0x74736554});
  ASSERT_NE(Name, 0u);
  success(Memory->write(Name, {'N', 'a', 'm', 'e', 0}));
  EXPECT_EQ(
      invoke("KeRegisterBugCheckReasonCallback", {Scratch, Entry, 2, Name}),
      1u);
  EXPECT_EQ(invoke("ExFreePool", {Name}), 0u);
  EXPECT_NE(denied(Name, 1).find("freed"), std::string::npos);
  EXPECT_EQ(integer(Scratch + 0x18), Name); // Never replaced by a copied name.
  success(Model->snapshot());
  ASSERT_EQ(Result.BugCheckCallbacks.size(), 1u);
  EXPECT_TRUE(Result.BugCheckCallbacks[0].ComponentStorageReleased);
  EXPECT_EQ(Result.BugCheckCallbacks[0].Component, Name);
  // Native deregistration removes the record without reading Component.
  EXPECT_EQ(invoke("KeDeregisterBugCheckReasonCallback", {Scratch}), 1u);
  success(Model->snapshot());
  EXPECT_TRUE(Result.BugCheckCallbacks.empty());
}

TEST_F(DriverNativeKernelModel, BugCheckNameRetirementSurvivesStackSlotReuse) {
  success(Memory->map(Entry, 0x1000, Read | Execute));
  constexpr uint64_t Stack = Scratch + 0x4000, Name = Stack + 0x100;
  success(Memory->write(Name, {'N', 0}));
  EXPECT_EQ(
      invoke("KeRegisterBugCheckReasonCallback", {Scratch, Entry, 4, Name}),
      1u);
  success(Model->validateGuestAccess(Name, 1, false));
  success(Model->snapshot());
  ASSERT_EQ(Result.BugCheckCallbacks.size(), 1u);
  EXPECT_FALSE(Result.BugCheckCallbacks[0].ComponentStorageReleased);
  success(Model->retireStack(Stack, 4096));
  EXPECT_NE(denied(Name, 1).find("freed"), std::string::npos);
  success(Model->activateStack(Stack, 4096));
  success(Memory->write(Name, {'X', 0}));
  success(Model->snapshot());
  ASSERT_EQ(Result.BugCheckCallbacks.size(), 1u);
  EXPECT_TRUE(Result.BugCheckCallbacks[0].ComponentStorageReleased);
  EXPECT_EQ(Result.BugCheckCallbacks[0].Component, Name);
  EXPECT_EQ(integer(Scratch + 0x18), Name);
  EXPECT_EQ(invoke("KeDeregisterBugCheckReasonCallback", {Scratch}), 1u);
}

TEST_F(DriverNativeKernelModel,
       RejectedBugCheckReleaseDoesNotPublishNameRetirement) {
  success(Memory->map(Entry, 0x1000, Read | Execute));
  const auto Record = invoke("ExAllocatePoolWithTag", {0, 128, 0x74736554});
  ASSERT_NE(Record, 0u);
  success(Memory->writeInteger(Record + 0x2c, 0, 1));
  success(Memory->write(Record + 64, {'N', 0}));
  EXPECT_EQ(invoke("KeRegisterBugCheckReasonCallback",
                   {Record, Entry, 2, Record + 64}),
            1u);
  EXPECT_NE(failure("ExFreePool", {Record}).find("retained"),
            std::string::npos);
  auto E = Model->retireStack(Entry, 4096);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("retained"), std::string::npos);
  success(Model->snapshot());
  ASSERT_EQ(Result.BugCheckCallbacks.size(), 1u);
  EXPECT_FALSE(Result.BugCheckCallbacks[0].ComponentStorageReleased);
  EXPECT_EQ(invoke("KeDeregisterBugCheckReasonCallback", {Record}), 1u);
  EXPECT_EQ(invoke("ExFreePool", {Record}), 0u);
}

TEST_F(DriverKernelModel, VersionQueryRequiresExplicitGuestFacts) {
  success(Memory->writeInteger(Scratch, 276, 4));
  EXPECT_NE(failure("RtlGetVersion", {Scratch}).find("windows_version"),
            std::string::npos);
  EXPECT_EQ(integer(Scratch, 4), 276u);
}

TEST(DriverVersion, ConfiguredVersionWritesBothWDKLayoutsWithoutTouchingTail) {
  for (unsigned Size : {0u, 276u, 284u}) {
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    auto &Memory = **Backend;
    DriverResult Result;
    KernelModel Model(Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    DriverOptions Options;
    Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    llvm::cantFail(Model.initialize(Image, Options));
    constexpr uint64_t Buffer = 0x60000000;
    llvm::cantFail(Memory.map(Buffer, 0x1000, Read | Write));
    llvm::cantFail(Memory.write(Buffer, std::vector<uint8_t>(300, 0xa5)));
    llvm::cantFail(Memory.writeInteger(Buffer, Size, 4));
    auto Status = Model.call("RtlGetVersion", {Buffer});
    ASSERT_TRUE(bool(Status)) << llvm::toString(Status.takeError());
    EXPECT_EQ(*Status, 0u);
    EXPECT_EQ(llvm::cantFail(Memory.readInteger(Buffer, 4)), Size);
    EXPECT_EQ(llvm::cantFail(Memory.readInteger(Buffer + 4, 4)), 10u);
    EXPECT_EQ(llvm::cantFail(Memory.readInteger(Buffer + 8, 4)), 0u);
    EXPECT_EQ(llvm::cantFail(Memory.readInteger(Buffer + 12, 4)), 19041u);
    EXPECT_EQ(llvm::cantFail(Memory.readInteger(Buffer + 16, 4)), 2u);
    EXPECT_EQ(llvm::cantFail(Memory.readInteger(Buffer + 20, 1)), 0xa5u);
    EXPECT_EQ(llvm::cantFail(Memory.readInteger(Buffer + 284, 1)), 0xa5u);
    if (Size == 284)
      EXPECT_EQ(llvm::cantFail(Memory.readInteger(Buffer + 282, 1)), 1u);
    llvm::cantFail(Memory.writeInteger(Buffer, 292, 4));
    auto Bad = Model.call("RtlGetVersion", {Buffer});
    ASSERT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
    EXPECT_EQ(llvm::cantFail(Memory.readInteger(Buffer + 4, 4)), 10u);
  }
}

TEST_F(DriverNativeKernelModel,
       SystemModuleProbeAndQueryUseTheSameLoadedIdentity) {
  Model->addReferenceModule(0xfffff80000000000, 0x1200000);
  constexpr uint64_t Length = Scratch + 0x1000;
  EXPECT_EQ(invoke("ZwQuerySystemInformation", {11, 0, 0, Length}),
            0xc0000004u);
  EXPECT_EQ(integer(Length, 4), 600u);
  EXPECT_EQ(invoke("ZwQuerySystemInformation", {11, Scratch, 600, Length}), 0u);
  EXPECT_EQ(integer(Scratch, 4), 2u);
  EXPECT_EQ(integer(Scratch + 8 + 16), 0xfffff80000000000u);
  EXPECT_EQ(integer(Scratch + 8 + 24, 4), 0x1200000u);
  EXPECT_EQ(integer(Scratch + 8 + 296 + 16), 0x180000000u);
  EXPECT_EQ(integer(Scratch + 8 + 296 + 24, 4), 0x3000u);
  EXPECT_EQ(integer(Length, 4), 600u);
  EXPECT_NE(failure("ZwQuerySystemInformation", {12, Scratch, 600, Length})
                .find("class: 12"),
            std::string::npos);
  EXPECT_EQ(integer(Scratch, 4), 2u);
  EXPECT_EQ(invoke("NtQuerySystemInformation", {11, 0, 0, Length}),
            0xc0000004u);
}

TEST_F(DriverNativeKernelModel,
       AnsiInitializationCountsBytesAndClearsRecordPadding) {
  success(Memory->write(Scratch + 0x100, {'N', 'D', 0}));
  success(Memory->write(Scratch, std::vector<uint8_t>(16, 0xa5)));
  invoke("RtlInitAnsiString", {Scratch, Scratch + 0x100});
  EXPECT_EQ(integer(Scratch, 2), 2u);
  EXPECT_EQ(integer(Scratch + 2, 2), 3u);
  EXPECT_EQ(integer(Scratch + 4, 4), 0u);
  EXPECT_EQ(integer(Scratch + 8), Scratch + 0x100);
  invoke("RtlInitAnsiString", {Scratch, 0});
  EXPECT_EQ(integer(Scratch, 4), 0u);
  EXPECT_EQ(integer(Scratch + 8), 0u);
}

TEST_F(DriverNativeKernelModel,
       AllocatedRTLConversionsHaveIndependentOwnedLifetimes) {
  constexpr uint64_t Ansi = Scratch, Wide = Scratch + 0x20;
  constexpr uint64_t Roundtrip = Scratch + 0x40, Text = Scratch + 0x100;
  success(Memory->write(Text, {'a', 'b', 'c', 0}));
  invoke("RtlInitAnsiString", {Ansi, Text});
  EXPECT_EQ(invoke("RtlAnsiStringToUnicodeString", {Wide, Ansi, 1}), 0u);
  EXPECT_EQ(integer(Wide, 2), 6u);
  EXPECT_EQ(integer(Wide + 2, 2), 8u);
  const uint64_t WideBuffer = integer(Wide + 8);
  EXPECT_EQ(integer(WideBuffer, 8), 0x006300620061u);
  EXPECT_EQ(invoke("RtlUnicodeStringToAnsiString", {Roundtrip, Wide, 1}), 0u);
  const uint64_t NarrowBuffer = integer(Roundtrip + 8);
  EXPECT_EQ(integer(NarrowBuffer, 4), 0x00636261u);
  invoke("RtlFreeUnicodeString", {Wide});
  EXPECT_EQ(integer(Wide, 8), 0u);
  EXPECT_EQ(integer(Wide + 8), 0u);
  EXPECT_EQ(integer(NarrowBuffer, 4), 0x00636261u);
  invoke("RtlFreeAnsiString", {Roundtrip});
  EXPECT_EQ(integer(Roundtrip + 8), 0u);
  EXPECT_EQ(integer(Text, 4), 0x00636261u);
  EXPECT_NE(denied(WideBuffer, 2).find("freed"), std::string::npos);
}

TEST_F(DriverNativeKernelModel,
       RTLShortBuffersMatchIndependentNativeASCIIObservations) {
  constexpr uint64_t Source = Scratch, Destination = Scratch + 0x20;
  constexpr uint64_t Input = Scratch + 0x100, Output = Scratch + 0x200;
  success(Memory->write(Input, {'a', 'b', 'c', 0}));
  invoke("RtlInitAnsiString", {Source, Input});
  for (unsigned Capacity : {0u, 2u, 6u, 7u, 8u}) {
    success(Memory->write(Output, std::vector<uint8_t>(16, 'Z')));
    success(Memory->writeInteger(Destination + 2, Capacity, 2));
    success(Memory->writeInteger(Destination + 8, Output, 8));
    EXPECT_EQ(invoke("RtlAnsiStringToUnicodeString", {Destination, Source, 0}),
              Capacity < 8 ? 0x80000005u : 0u);
    EXPECT_EQ(integer(Destination, 2), 6u);
    EXPECT_EQ(integer(Output, 8),
              Capacity < 8 ? 0x5a5a5a5a5a5a5a5au : 0x006300620061u);
  }
  success(Memory->write(Input, {'a', 0, 'b', 0, 'c', 0, 0, 0}));
  invoke("RtlInitUnicodeString", {Source, Input});
  for (unsigned Capacity : {0u, 1u, 2u, 3u, 4u}) {
    success(Memory->write(Output, std::vector<uint8_t>(16, 'Z')));
    success(Memory->writeInteger(Destination + 2, Capacity, 2));
    success(Memory->writeInteger(Destination + 8, Output, 8));
    EXPECT_EQ(invoke("RtlUnicodeStringToAnsiString", {Destination, Source, 0}),
              Capacity < 4 ? 0x80000005u : 0u);
    EXPECT_EQ(integer(Destination, 2),
              Capacity ? std::min(3u, Capacity - 1) : 3u);
    EXPECT_EQ(integer(Output + (Capacity ? std::min(3u, Capacity - 1) : 0), 1),
              Capacity ? 0u : uint64_t('Z'));
  }
}

TEST_F(DriverNativeKernelModel, RTLFreeRejectsARecordInsideItsOwnedBuffer) {
  const uint64_t Source = Scratch, Destination = Scratch + 0x20;
  const uint64_t Text = Scratch + 0x100;
  success(Memory->write(Text, std::vector<uint8_t>(40, 'a')));
  success(Memory->writeInteger(Text + 40, 0, 1));
  invoke("RtlInitAnsiString", {Source, Text});
  invoke("RtlAnsiStringToUnicodeString", {Destination, Source, 1});
  const auto Buffer = integer(Destination + 8);
  success(Memory->writeInteger(Buffer + 8, Buffer, 8));
  EXPECT_NE(failure("RtlFreeUnicodeString", {Buffer}).find("overlaps"),
            std::string::npos);
  success(Model->validateGuestAccess(Buffer, 16, false));
  invoke("RtlFreeUnicodeString", {Destination});
  EXPECT_NE(denied(Buffer, 16).find("freed"), std::string::npos);
}

TEST_F(DriverNativeKernelModel,
       PhysicalRangeInventoryRequiresExplicitObservation) {
  EXPECT_NE(failure("MmGetPhysicalMemoryRanges", {})
                .find("physical_memory_inventory"),
            std::string::npos);
  const auto Pool = invoke("ExAllocatePool", {0, 16});
  ASSERT_NE(Pool, 0u);
  invoke("ExFreePool", {Pool});
  EXPECT_EQ(invoke("ZwQuerySystemInformation", {196, Scratch, 4, Scratch + 8}),
            0u);
  EXPECT_EQ(integer(Scratch, 4), 0u);
  EXPECT_EQ(integer(Scratch + 8, 4), 4u);
}

TEST_F(DriverNativeKernelModel,
       ThreadTerminationRejectsUnknownPointersAndTracksIRQL) {
  Model->enterExecution(profile::StackBase);
  const auto Object = invoke("PsGetCurrentThread", {});
  for (auto Invalid : {uint64_t(0), Scratch, Object + 1, UINT64_MAX})
    EXPECT_NE(failure("PsIsThreadTerminating", {Invalid}).find("known thread"),
              std::string::npos);
  EXPECT_EQ(invoke("PsIsThreadTerminating", {Object}), 0u);
  EXPECT_NE(failure("ObfDereferenceObject", {Object}).find("referenced thread"),
            std::string::npos);
  EXPECT_NE(failure("PsIsThreadTerminating", {}).find("argument count"),
            std::string::npos);
  invoke("KfRaiseIrql", {2});
  EXPECT_EQ(invoke("PsIsThreadTerminating", {Object}), 0u);
  invoke("KfRaiseIrql", {3});
  EXPECT_NE(failure("PsIsThreadTerminating", {Object}).find("IRQL"),
            std::string::npos);
  invoke("KeLowerIrql", {2});
  invoke("KeLowerIrql", {0});
}

TEST_F(DriverNativeKernelModel,
       ThreadTerminationRemainsSetAcrossSystemExitAndReferences) {
  Model->enterExecution(profile::StackBase);
  ASSERT_EQ(invoke("PsCreateSystemThread",
                   {Scratch, windows::ThreadAllAccess, 0, 0, 0, Entry, 0}),
            0u);
  const auto Handle = integer(Scratch);
  ASSERT_EQ(
      invoke("ObReferenceObjectByHandle", {Handle, 0, 0, 0, Scratch + 8, 0}),
      0u);
  const auto Object = integer(Scratch + 8);
  EXPECT_EQ(invoke("PsIsThreadTerminating", {Object}), 0u);
  auto Next = llvm::cantFail(Model->nextScheduled(false));
  ASSERT_TRUE(Next);
  Model->enterExecution(Scratch + 0x8000, Next->ID);
  EXPECT_EQ(invoke("PsIsThreadTerminating", {Object}), 0u);
  invoke("PsTerminateSystemThread", {0x1234});
  EXPECT_EQ(invoke("PsIsThreadTerminating", {Object}), 1u);
  ASSERT_EQ(Model->takeThreadTermination(), 0x1234u);
  success(Model->finishScheduled(Next->ID));
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(invoke("PsIsThreadTerminating", {Object}), 1u);
  invoke("ZwClose", {Handle});
  EXPECT_EQ(invoke("PsIsThreadTerminating", {Object}), 1u);
  invoke("ObfDereferenceObject", {Object});
  EXPECT_NE(failure("PsIsThreadTerminating", {Object}).find("known thread"),
            std::string::npos);
}

TEST_F(DriverNativeKernelModel,
       ThreadTerminationRetainsExitedBorrowedIdentity) {
  constexpr uint64_t Stack = Scratch + 0x8000;
  Model->enterExecution(Stack);
  const auto Object = invoke("PsGetCurrentThread", {});
  const auto ID = invoke("PsGetThreadId", {Object});
  ASSERT_EQ(invoke("PsLookupThreadByThreadId", {ID, Scratch}), 0u);
  EXPECT_EQ(invoke("PsIsThreadTerminating", {Object}), 0u);
  success(Model->retireStack(Stack, profile::PageSize));
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(invoke("PsIsThreadTerminating", {Object}), 1u);
  EXPECT_EQ(invoke("PsLookupThreadByThreadId", {ID, Scratch}),
            windows::StatusInvalidCID);
  invoke("ObfDereferenceObject", {Object});
  EXPECT_NE(failure("PsIsThreadTerminating", {Object}).find("known thread"),
            std::string::npos);
}

TEST(DriverThreadTermination,
     ImportedAndDynamicQueriesFollowRealThreadLifetime) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_thread_termination.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
    }
}

} // namespace
} // namespace neverd::emulation
