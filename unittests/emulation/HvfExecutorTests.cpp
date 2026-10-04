//===- HvfExecutorTests.cpp - Ownership, rollback and native cancellation ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#if defined(__APPLE__) && defined(NEVERD_EMULATION_HVF)
#include "arch/aarch64/AArch64Machine.h"
#include "arch/x86_64/X64Machine.h"
#include "backends/MachineFactories.h"
#include "backends/hvf/HvfExecutor.h"
#include "core/ExecutionDiagnostics.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>
#include <future>
#include <system_error>
#if defined(__x86_64__)
#include <Hypervisor/hv_vmx.h>
#include <mach/mach_time.h>
#include <pthread.h>
#endif

#if defined(__x86_64__)
namespace neverd::emulation::hvf {
// Test-only access keeps the lifecycle experiment out of the production API.
struct ExecutorProbe {
  enum class Fault {
    None,
    BeforeReplacement,
    BeforeOldDestroy,
    BeforeNewCreate
  };
  static void ownerMarker(const char *Phase, uint64_t Generation,
                          uint64_t Owner) {
    llvm::outs() << "INTEL_OWNER phase=" << Phase
                 << " vcpu_generation=" << Generation
                 << " owner_generation=" << Generation << " owner=" << Owner
                 << " vm_generation=1\n";
    llvm::outs().flush();
  }
  static llvm::Error checkOwner(Executor &Host, uint64_t &Owner, bool HasCPU) {
    return Host.submit([&](Executor &H) -> llvm::Error {
      uint64_t Current = 0;
      if (pthread_threadid_np(nullptr, &Current) || !Current ||
          (Owner && Current != Owner) || H.Mapped || H.Active ||
          !H.Failure.empty() || !H.VMCreated || H.CPUCreated != HasCPU ||
          !H.VMLease.owns_lock())
        return diagnostic::error("owner handoff state mismatch");
      Owner = Current;
      return llvm::Error::success();
    });
  }
  static llvm::Error recreateOwner(Executor &Host, uint64_t Generation,
                                   uint64_t &Owner, Fault Inject = Fault::None,
                                   bool Trace = true) {
    struct Handoff {
      std::mutex Mutex;
      std::condition_variable Changed;
      bool Start = false, Cancel = false;
      uint64_t Owner = 0;
      std::promise<void> Parked;
      std::promise<llvm::Error> Ready;
    };
    auto State = std::make_shared<Handoff>();
    auto Parked = State->Parked.get_future();
    auto Ready = State->Ready.get_future();
    std::thread Replacement;
    try {
      if (Inject == Fault::BeforeReplacement)
        throw std::system_error(
            std::make_error_code(std::errc::resource_unavailable_try_again));
      Replacement = std::thread([State, &Host, Generation, Inject, Trace] {
        // Precreation can fail without leaving Host without a Worker. Until
        // Start, this thread must not access Host or any native HVF resource.
        if (pthread_threadid_np(nullptr, &State->Owner))
          State->Owner = 0;
        State->Parked.set_value();
        {
          std::unique_lock Lock(State->Mutex);
          State->Changed.wait(Lock,
                              [&] { return State->Start || State->Cancel; });
          if (State->Cancel)
            return;
        }
        if (Trace)
          ownerMarker("owner_activate", Generation + 1, State->Owner);
        Host.VMLease.lock(); // The old owner released this same mutex.
        if (Trace)
          ownerMarker("lease_acquire_end", Generation + 1, State->Owner);
        llvm::Error Result = llvm::Error::success();
        if (Inject == Fault::BeforeNewCreate) {
          Result = diagnostic::error("injected before new CPU creation");
        } else {
          if (Trace)
            ownerMarker("vcpu_create_begin", Generation + 1, State->Owner);
          Result = Host.createCPU();
          if (!Result && Trace)
            ownerMarker("vcpu_create_end", Generation + 1, State->Owner);
        }
        State->Ready.set_value(std::move(Result));
        // Even failed initialization must service the original destructor.
        Host.work();
      });
    } catch (const std::system_error &) {
      return diagnostic::error("owner replacement thread creation failed");
    }
    auto Cancel = llvm::scope_exit([&] {
      if (Replacement.joinable()) {
        {
          std::lock_guard Lock(State->Mutex);
          State->Cancel = true;
        }
        State->Changed.notify_one();
        Replacement.join();
      }
    });
    Parked.get();
    if (!State->Owner)
      return diagnostic::error("replacement owner identity unavailable");
    if (Trace)
      ownerMarker("replacement_parked", Generation + 1, State->Owner);
    if (auto E = Host.submit([&](Executor &H) -> llvm::Error {
          uint64_t Current = 0;
          if (pthread_threadid_np(nullptr, &Current) || !Current ||
              Current == State->Owner || (Owner && Owner != Current) ||
              H.Mapped || H.Active || !H.Failure.empty() || !H.VMCreated ||
              !H.CPUCreated || !H.VMLease.owns_lock())
            return diagnostic::error("owner reset requires an idle live VM");
          Owner = Current;
          if (Inject == Fault::BeforeOldDestroy)
            return diagnostic::error("injected before old CPU destruction");
          if (Trace)
            ownerMarker("vcpu_destroy_begin", Generation, Owner);
          if (auto Error = H.destroyCPU())
            return Error;
          if (Trace)
            ownerMarker("vcpu_destroy_end", Generation, Owner);
          H.VMLease.unlock();
          if (Trace)
            ownerMarker("lease_release_end", Generation, Owner);
          return llvm::Error::success();
        }))
      return E; // Cancel/join the parked replacement; old Worker remains live.
    // This diagnostic has one submitter. submit must finish before Admission
    // is acquired: taking it inside submit would deadlock its nonrecursive
    // lock.
    std::unique_lock Gate(Host.Admission);
    {
      std::lock_guard Lock(Host.Mutex);
      if (Host.Pending || Host.Shutdown)
        std::abort();
      Host.Shutdown = true;
      Host.Changed.notify_one();
    }
    if (Trace)
      ownerMarker("owner_join_begin", Generation, Owner);
    Host.Worker.join(); // Never join while holding Host.Mutex.
    if (Trace)
      ownerMarker("owner_join_end", Generation, Owner);
    {
      std::lock_guard Lock(Host.Mutex);
      Host.Shutdown = false;
      Host.Worker = std::move(Replacement);
    }
    {
      std::lock_guard Lock(State->Mutex);
      State->Start = true;
    }
    State->Changed.notify_one();
    auto Result = Ready.get();
    Owner = State->Owner;
    return Result;
  }
  static void marker(const char *Phase, uint64_t Generation, uint64_t Owner) {
    llvm::outs() << "INTEL_LIFECYCLE phase=" << Phase
                 << " generation=" << Generation << " owner=" << Owner << '\n';
    llvm::outs().flush();
  }
  static llvm::Error recreate(Executor &Host, uint64_t Generation,
                              uint64_t &Owner, bool RecreateVM) {
    return Host.submit([&](Executor &H) -> llvm::Error {
      uint64_t Current = 0;
      if (pthread_threadid_np(nullptr, &Current) || !Current)
        return diagnostic::error("lifecycle owner identity unavailable");
      if (Owner && Owner != Current)
        return diagnostic::error("lifecycle owner changed");
      Owner = Current;
      if (H.Mapped || H.Active || !H.Failure.empty() || !H.VMCreated ||
          !H.CPUCreated || !H.VMLease.owns_lock())
        return diagnostic::error("lifecycle reset requires an idle live VM");
      marker("vcpu_destroy_begin", Generation, Owner);
      if (auto E = H.destroyCPU())
        return E;
      marker("vcpu_destroy_end", Generation, Owner);
      if (RecreateVM) {
        // Intel RunControl contains only the Mach timebase. Keep it and the
        // VM lease while changing only the native VM's lifetime on this owner.
        marker("vm_destroy_begin", Generation, Owner);
        if (auto S = hv_vm_destroy())
          return error("probe hv_vm_destroy", S);
        H.VMCreated = false;
        marker("vm_destroy_end", Generation, Owner);
        marker("vm_create_begin", Generation + 1, Owner);
        if (auto S = hv_vm_create(HV_VM_DEFAULT))
          return unavailable("probe hv_vm_create", S);
        H.VMCreated = true;
        marker("vm_create_end", Generation + 1, Owner);
      }
      marker("vcpu_create_begin", Generation + 1, Owner);
      if (auto E = H.createCPU())
        return E;
      marker("vcpu_create_end", Generation + 1, Owner);
      return llvm::Error::success();
    });
  }
};
} // namespace neverd::emulation::hvf
#endif

namespace neverd::emulation {
namespace {
using Clock = std::chrono::steady_clock;
#if defined(__x86_64__)
bool probeFlag(const char *Name) {
  const auto *Value = std::getenv(Name);
  return Value && llvm::StringRef(Value) == "1";
}
struct RetainedExecutor {
  std::shared_ptr<hvf::Executor> Host;
  uint64_t Recreations = 0, Owner = 0;
  bool RecreateCPU = false, RecreateVM = false, RecreateOwner = false;
  ~RetainedExecutor() {
    if (RecreateCPU) {
      // No fixture or machine may keep the worker alive beyond this marker.
      if (Host.use_count() != 1)
        std::abort();
      const std::weak_ptr<hvf::Executor> Retiring = Host;
      auto Marker = RecreateOwner ? hvf::ExecutorProbe::ownerMarker
                                  : hvf::ExecutorProbe::marker;
      Marker("executor_retire_begin", Recreations + 1, Owner);
      Host.reset(); // Original native teardown followed by Worker.join().
      if (!Retiring.expired())
        std::abort();
      Marker("executor_retire_end", Recreations + 1, Owner);
    }
  }
};
#endif
class HvfExecutor : public testing::Test {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::shared_ptr<hvf::Executor> Host;
#if defined(__x86_64__)
  void checkIntelRecovery(bool WithHostKick);
  RetainedExecutor *Retained = nullptr;
  void TearDown() override {
    if (Retained && Retained->RecreateOwner)
      EXPECT_EQ(llvm::toString(hvf::ExecutorProbe::recreateOwner(
                    *Host, ++Retained->Recreations, Retained->Owner)),
                "");
    else if (Retained && Retained->RecreateCPU)
      EXPECT_EQ(llvm::toString(hvf::ExecutorProbe::recreate(
                    *Host, ++Retained->Recreations, Retained->Owner,
                    Retained->RecreateVM)),
                "");
  }
#endif
  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(4096));
    auto Created = hvf::Executor::acquire();
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Text = llvm::toString(std::move(E));
      if (Unavailable && !std::getenv("NEVERD_REQUIRE_HVF"))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    Host = *Created;
#if defined(__x86_64__)
    const auto *Probe = std::getenv("NEVERD_HVF_INTEL_PROBE");
    const auto *Reuse = std::getenv("NEVERD_HVF_INTEL_REUSE_EXECUTOR");
    if (Probe && llvm::StringRef(Probe) == "1" && Reuse &&
        llvm::StringRef(Reuse) == "1") {
      // Diagnostic-only lifetime control; VM and executor threads survive.
      // The optional probes recreate vCPU or VM after detaching mappings.
      // Every fixture still owns fresh memory. This
      // static is initialized after the native registry/VM mutexes and is
      // destroyed before them when the diagnostic process exits.
      auto &Output = llvm::outs(); // Outlive the optional final trace.
      static RetainedExecutor RetainedHost{
          Host,
          0,
          0,
          probeFlag("NEVERD_HVF_INTEL_RECREATE_VCPU"),
          probeFlag("NEVERD_HVF_INTEL_RECREATE_VM"),
          probeFlag("NEVERD_HVF_INTEL_RECREATE_OWNER")};
      ASSERT_TRUE(!RetainedHost.RecreateVM || RetainedHost.RecreateCPU);
      ASSERT_TRUE(!RetainedHost.RecreateOwner ||
                  (RetainedHost.RecreateCPU && !RetainedHost.RecreateVM));
      ASSERT_EQ(Host, RetainedHost.Host);
      Retained = &RetainedHost;
      Output << "INTEL_PROBE phase=executor_retained\n";
      Output.flush();
    }
#endif
  }
  MachineRunControl control() {
    return {Clock::now() + std::chrono::seconds(2)};
  }
};
TEST_F(HvfExecutor, OneOwnerThreadAndCancelledQueueAdmissionAllowsRetry) {
  hvf::Binding First(Host, *Memory), Second(Host, *Memory);
  std::promise<void> Entered, Release;
  auto EnteredFuture = Entered.get_future();
  auto ReleaseFuture = Release.get_future();
  std::thread::id Owner, NextOwner;
  std::thread Caller([&] {
    EXPECT_EQ(llvm::toString(First.execute(
                  control(),
                  [&](auto &) -> llvm::Error {
                    Owner = std::this_thread::get_id();
                    Entered.set_value();
                    if (ReleaseFuture.wait_for(std::chrono::seconds(2)) !=
                        std::future_status::ready)
                      return diagnostic::error("test owner was not released");
                    return llvm::Error::success();
                  })),
              "");
  });
  EXPECT_EQ(EnteredFuture.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  std::atomic<bool> Stop{true};
  bool Invoked = false;
  auto E = Second.execute({Clock::now() + std::chrono::seconds(2), &Stop},
                          [&](auto &) -> llvm::Error {
                            Invoked = true;
                            return llvm::Error::success();
                          });
  EXPECT_TRUE(E.isA<MachineInterruptedError>());
  llvm::consumeError(std::move(E));
  EXPECT_FALSE(Invoked);
  Release.set_value();
  Caller.join();
  EXPECT_EQ(llvm::toString(Second.execute(control(),
                                          [&](auto &) -> llvm::Error {
                                            NextOwner =
                                                std::this_thread::get_id();
                                            return llvm::Error::success();
                                          })),
            "");
  EXPECT_EQ(Owner, NextOwner);
  EXPECT_NE(Owner, std::this_thread::get_id());
}
TEST_F(HvfExecutor, PartialMappingFailureRetiresBorrowedBacking) {
  auto Registrations = Memory->registrations();
  // Both regions are valid host allocations, but the second GPA overlaps the
  // first. Exercise native mapping failure after the first mapping succeeds.
  Registrations[1].Physical = Registrations[0].Physical;
  bool Invoked = false;
  auto E =
      Host->execute(this, Registrations, control(), [&](auto &) -> llvm::Error {
        Invoked = true;
        return llvm::Error::success();
      });
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_FALSE(Invoked);
  Memory.reset();
  Host.reset();
  auto Recreated = hvf::Executor::acquire();
  EXPECT_TRUE(bool(Recreated)) << llvm::toString(Recreated.takeError());
}
#if defined(__arm64__)
AArch64MachineState maintenanceState(bool User, uint64_t PC) {
  AArch64MachineState State;
  State.UserMode = User;
  for (unsigned N = 0; N < State.Registers.size(); ++N)
    State.Registers[N] = 0x1234567800000000ull + N;
  for (unsigned N = 0; N < State.Vectors.size(); ++N)
    State.Vectors[N] = {0x8765432100000000ull + N, 0xfedcba9800000000ull - N};
  State.reg(AArch64Register::PC) = PC;
  State.reg(AArch64Register::SP) = PC + 0xff0;
  State.reg(AArch64Register::NZCV) = 0xa0000000;
  State.reg(AArch64Register::FPCR) = 0x01400000;
  State.reg(AArch64Register::FPSR) = 0x08000015;
  return State;
}

TEST_F(HvfExecutor, MaintenanceFaultsDoNotPublishAndAllowRetry) {
  auto Machine = llvm::cantFail(createHvfAArch64Machine(*Memory));
  constexpr uint64_t Code = 0x10000;
  llvm::cantFail(
      Memory->map(Code, 4096, Read | Write | Execute | UserAccessible));
  const uint8_t Nop[] = {0x1f, 0x20, 0x03, 0xd5};
  llvm::cantFail(Memory->write(Code, Nop));
  for (bool User : {false, true}) {
    SCOPED_TRACE(User);
    auto State = maintenanceState(User, Code);
    llvm::cantFail(Memory->beginRun());
    auto Release = llvm::scope_exit([&] { Memory->endRun(); });
    llvm::cantFail(buildAArch64PageTables(*Memory, User));
    auto *Tail = Memory->data() + aarch64::MaintenanceExitGPA;
    // Change only instructions AFTER IC/DSB/ISB: startup has already warmed
    // this stub, and an injection before cache invalidation may be invisible.
    for (uint32_t Word : {
             0u,          // UDF reaches the normal #0 fault vector
             0xd4000002u, // HVC #0
             0xd4000042u, // HVC #2
             0x14000002u  // branch to HVC #1 at the wrong PC
         }) {
      SCOPED_TRACE(Word);
      State.reg(AArch64Register::PC) = Code;
      const auto Before = State;
      llvm::support::endian::write32le(Tail, Word);
      llvm::support::endian::write32le(Tail + 8, 0xd4000022);
      auto E = Machine->step(State, control());
      EXPECT_TRUE(bool(E));
      EXPECT_FALSE(E.isA<MachineInterruptedError>());
      llvm::consumeError(std::move(E));
      EXPECT_EQ(State.Registers, Before.Registers);
      EXPECT_EQ(State.Vectors, Before.Vectors);
      EXPECT_EQ(State.UserMode, Before.UserMode);

      llvm::support::endian::write32le(Tail, aarch64::MaintenanceHypercall);
      llvm::support::endian::write32le(Tail + 8, 0);
      auto Expected = Before;
      Expected.reg(AArch64Register::PC) += 4;
      ASSERT_EQ(llvm::toString(Machine->step(State, control())), "");
      EXPECT_EQ(State.Registers, Expected.Registers);
      EXPECT_EQ(State.Vectors, Expected.Vectors);
      EXPECT_EQ(State.UserMode, Expected.UserMode);
    }
  }
}

TEST_F(HvfExecutor, MaintenanceCancellationDoesNotPublishAndAllowsCodeRewrite) {
  auto Machine = llvm::cantFail(createHvfAArch64Machine(*Memory));
  constexpr uint64_t Code = 0x10000;
  llvm::cantFail(
      Memory->map(Code, 4096, Read | Write | Execute | UserAccessible));
  // The injected native STR is an execution witness. The stopper reads only
  // this aligned word, never the core API/lock held by the submitting thread.
  auto *Scratch =
      Memory->physicalPointer(Memory->mappings().at(Code).Physical) + 0x100;
  static_assert(std::atomic<uint64_t>::is_always_lock_free);
  auto *Witness = std::construct_at(
      reinterpret_cast<std::atomic<uint64_t> *>(Scratch), uint64_t(0));
  auto DestroyWitness = llvm::scope_exit([&] { std::destroy_at(Witness); });
  for (bool User : {false, true}) {
    for (bool Deadline : {true, false}) {
      SCOPED_TRACE(User);
      SCOPED_TRACE(Deadline);
      auto State = maintenanceState(User, Code);
      State.reg(AArch64Register::X1) = Code + 0x100;
      const uint8_t Nop[] = {0x1f, 0x20, 0x03, 0xd5};
      llvm::cantFail(Memory->write(Code, Nop));
      auto Step = [&](uint32_t Tail, MachineRunControl Control) {
        llvm::cantFail(Memory->beginRun());
        auto Release = llvm::scope_exit([&] { Memory->endRun(); });
        llvm::cantFail(buildAArch64PageTables(*Memory, User));
        llvm::support::endian::write32le(
            Memory->data() + aarch64::MaintenanceExitGPA, Tail);
        llvm::support::endian::write32le(
            Memory->data() + aarch64::MaintenanceExitGPA + 4,
            Tail == 0xf9000020 ? 0x14000000 : 0); // str witness; b .
        return Machine->step(State, Control);
      };
      // Warm the guest NOP too; the retry must observe its replacement.
      ASSERT_EQ(llvm::toString(Step(aarch64::MaintenanceHypercall, control())),
                "");
      State.reg(AArch64Register::PC) = Code;
      const auto Before = State;
      Witness->store(0);
      std::atomic<bool> Stop{false}, Done{false};
      std::thread Stopper;
      if (!Deadline)
        Stopper = std::thread([&] {
          while (!Witness->load() && !Done.load())
            std::this_thread::yield();
          if (Witness->load())
            Stop = true;
        });
      auto E =
          Step(0xf9000020, // str x0, [x1]; then loop inside maintenance
               {Clock::now() + std::chrono::milliseconds(Deadline ? 50 : 2000),
                &Stop});
      Done = true;
      if (Stopper.joinable())
        Stopper.join();
      EXPECT_TRUE(E.isA<MachineInterruptedError>());
      llvm::consumeError(std::move(E));
      EXPECT_EQ(Witness->load(), Before.reg(AArch64Register::X0));
      EXPECT_EQ(State.Registers, Before.Registers);
      EXPECT_EQ(State.Vectors, Before.Vectors);
      EXPECT_EQ(State.UserMode, Before.UserMode);

      const uint8_t Move[] = {0x40, 0x05, 0x80, 0xd2}; // mov x0, #42
      llvm::cantFail(Memory->write(Code, Move));
      auto Expected = Before;
      Expected.reg(AArch64Register::X0) = 42;
      Expected.reg(AArch64Register::PC) += 4;
      ASSERT_EQ(llvm::toString(Step(aarch64::MaintenanceHypercall, control())),
                "");
      EXPECT_EQ(State.Registers, Expected.Registers);
      EXPECT_EQ(State.Vectors, Expected.Vectors);
      EXPECT_EQ(State.UserMode, Expected.UserMode);
    }
  }
}

TEST_F(HvfExecutor,
       GuestFaultDoesNotPublishAtEitherARMPrivilegeAndAllowsRetry) {
  auto Machine = llvm::cantFail(createHvfAArch64Machine(*Memory));
  constexpr uint64_t Code = 0x10000;
  llvm::cantFail(
      Memory->map(Code, 4096, Read | Write | Execute | UserAccessible));
  for (bool User : {false, true}) {
    AArch64MachineState State;
    State.UserMode = User;
    State.reg(AArch64Register::PC) = Code;
    State.reg(AArch64Register::X0) = 0x12345678;
    State.Vectors[31] = {0x1234, 0x5678};
    auto Step = [&](uint32_t Instruction) {
      uint8_t Bytes[4];
      llvm::support::endian::write32le(Bytes, Instruction);
      llvm::cantFail(Memory->write(Code, Bytes));
      llvm::cantFail(Memory->beginRun());
      auto Release = llvm::scope_exit([&] { Memory->endRun(); });
      llvm::cantFail(buildAArch64PageTables(*Memory, User));
      return Machine->step(State, control());
    };
    const auto Before = State;
    auto E = Step(0); // UDF #0 raises an architectural exception.
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(llvm::toString(Step(0xd503201f)), ""); // nop retry
    EXPECT_EQ(State.reg(AArch64Register::PC), Code + 4);
    EXPECT_EQ(State.reg(AArch64Register::X0), Before.reg(AArch64Register::X0));
  }
}
llvm::Error prepareRaw(hvf::Executor &Host, uint64_t PC) {
  const auto CPU = Host.cpu();
  for (auto [Reg, Value] :
       {std::pair{HV_SYS_REG_SCTLR_EL1, uint64_t(0x30d00800)},
        {HV_SYS_REG_MDSCR_EL1, 0},
        {HV_SYS_REG_CNTV_CTL_EL0, 0}})
    if (auto S = hv_vcpu_set_sys_reg(CPU, Reg, Value))
      return hvf::error("test set_sys_reg", S);
  if (auto S = hv_vcpu_set_trap_debug_exceptions(CPU, false))
    return hvf::error("test trap debug", S);
  if (auto S = hv_vcpu_set_reg(CPU, HV_REG_CPSR, 0x3c5))
    return hvf::error("test CPSR", S);
  if (auto S = hv_vcpu_set_reg(CPU, HV_REG_PC, PC))
    return hvf::error("test PC", S);
  return llvm::Error::success();
}
TEST_F(HvfExecutor, NativeLoopDeadlineAndStopRetireInterruptsBeforeRetry) {
  constexpr uint64_t PC = 8192, RetryPC = PC + 64;
  // This raw transport fixture has no guest I-cache maintenance. Keep both
  // programs immutable: replacing the HVC with B at the same PC can execute
  // a stale HVC after migration and falsely report failed cancellation.
  llvm::support::endian::write32le(Memory->data() + PC, 0x14000000); // b .
  llvm::support::endian::write32le(Memory->data() + RetryPC,
                                   0xd4000002); // hvc #0
  hvf::Binding Binding(Host, *Memory);
  for (bool Deadline : {true, false}) {
    SCOPED_TRACE(Deadline ? "deadline" : "stop token");
    std::atomic<bool> Stop{false};
    std::promise<void> Entered;
    auto Ready = Entered.get_future();
    std::thread Stopper;
    if (!Deadline)
      Stopper = std::thread([&] {
        Ready.wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Stop = true;
      });
    auto Control = MachineRunControl{
        Clock::now() + std::chrono::milliseconds(Deadline ? 50 : 2000), &Stop};
    bool NativeReturned = false;
    auto E = Binding.execute(Control, [&](auto &Native) -> llvm::Error {
      auto Prepared = prepareRaw(Native, PC);
      Entered.set_value();
      if (Prepared)
        return Prepared;
      return Native.run(Control, [&](bool Cancelled) {
        NativeReturned = true;
        EXPECT_TRUE(Cancelled);
        EXPECT_EQ(Native.exit().reason, HV_EXIT_REASON_CANCELED)
            << "syndrome=" << Native.exit().exception.syndrome;
        return llvm::Error::success();
      });
    });
    if (Ready.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready)
      Entered.set_value();
    if (Stopper.joinable())
      Stopper.join();
    EXPECT_TRUE(NativeReturned);
    EXPECT_TRUE(E.isA<MachineInterruptedError>())
        << llvm::toString(std::move(E));
    llvm::consumeError(std::move(E));
    // Recreated vCPU must enter a different instruction, with no old kick.
    E = Binding.execute(control(), [&](auto &Native) -> llvm::Error {
      if (auto E = prepareRaw(Native, RetryPC))
        return E;
      return Native.run(control(), [&](bool Cancelled) {
        EXPECT_FALSE(Cancelled);
        EXPECT_EQ(Native.exit().reason, HV_EXIT_REASON_EXCEPTION);
        EXPECT_EQ(Native.exit().exception.syndrome >> 26, 0x16u);
        return llvm::Error::success();
      });
    });
    EXPECT_EQ(llvm::toString(std::move(E)), "");
  }
}
TEST_F(HvfExecutor, CompletionFailureOutranksConcurrentStop) {
  constexpr uint64_t PC = 8192;
  llvm::support::endian::write32le(Memory->data() + PC, 0xd4000002);
  hvf::Binding Binding(Host, *Memory);
  std::atomic<bool> Stop{false};
  auto Control = control();
  Control.Stop = &Stop;
  auto E = Binding.execute(Control, [&](auto &Native) -> llvm::Error {
    if (auto E = prepareRaw(Native, PC))
      return E;
    return Native.run(Control, [&](bool) {
      Stop = true;
      return diagnostic::error("injected complete-state capture failure");
    });
  });
  EXPECT_FALSE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)),
            "injected complete-state capture failure");
}
#endif
#if defined(__x86_64__)
// Opt-in diagnostics are not production acceptance requirements. In particular,
// a finite API return is not yet a rule for authenticating guest completion.
class HvfIntelHandoff : public testing::Test {
  void SetUp() override {
    if (!probeFlag("NEVERD_HVF_INTEL_PROBE"))
      GTEST_SKIP() << "explicit Intel diagnostic experiment required";
    for (const char *Name :
         {"NEVERD_HVF_INTEL_REUSE_EXECUTOR", "NEVERD_HVF_INTEL_RECREATE_VCPU",
          "NEVERD_HVF_INTEL_RECREATE_VM", "NEVERD_HVF_INTEL_RECREATE_OWNER"})
      ASSERT_FALSE(probeFlag(Name));
  }
};
TEST_F(HvfIntelHandoff, FailureControls) {
  using Probe = hvf::ExecutorProbe;
  struct Case {
    Probe::Fault Fault;
    const char *Name, *Error;
  };
  const Case Cases[] = {{Probe::Fault::BeforeReplacement, "replacement",
                         "owner replacement thread creation failed"},
                        {Probe::Fault::BeforeOldDestroy, "old_destroy",
                         "injected before old CPU destruction"},
                        {Probe::Fault::BeforeNewCreate, "new_create",
                         "injected before new CPU creation"}};
  for (auto [Fault, Name, Error] : Cases) {
    const auto Marker = [&](const char *Phase) {
      llvm::outs() << "INTEL_HANDOFF_CONTROL case=" << Name
                   << " phase=" << Phase << '\n';
      llvm::outs().flush();
    };
    Marker("begin");
    auto Created = hvf::Executor::acquire();
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    auto Host = std::move(*Created);
    uint64_t Owner = 0;
    ASSERT_EQ(llvm::toString(Probe::checkOwner(*Host, Owner, true)), "");
    ASSERT_EQ(
        llvm::toString(Probe::recreateOwner(*Host, 1, Owner, Fault, false)),
        Error);
    // Read state through the surviving worker without auto-creating a CPU or
    // executing a guest. New-create failure leaves CPUCreated false.
    ASSERT_EQ(llvm::toString(Probe::checkOwner(
                  *Host, Owner, Fault != Probe::Fault::BeforeNewCreate)),
              "");
    Marker("state_checked");
    ASSERT_EQ(Host.use_count(), 1);
    const std::weak_ptr<hvf::Executor> Retiring = Host;
    Host.reset(); // Original teardown must remain responsive after any fault.
    ASSERT_TRUE(Retiring.expired());
    Marker("cleanup_end");
  }
}

class HvfIntelProbe : public HvfExecutor {
  void SetUp() override {
    const auto *Enabled = std::getenv("NEVERD_HVF_INTEL_PROBE");
    if (!Enabled || llvm::StringRef(Enabled) != "1")
      GTEST_SKIP() << "explicit Intel diagnostic experiment required";
    llvm::outs() << "INTEL_PROBE phase=executor_initialization\n";
    llvm::outs().flush();
    HvfExecutor::SetUp();
  }
};

void probePhase(const char *Phase) {
  llvm::outs() << "INTEL_PROBE phase=" << Phase << '\n';
  llvm::outs().flush();
}

TEST_F(HvfIntelProbe, LifecycleOnly) {
  // The machine factory executes startup instructions: do not call it here.
  probePhase("lifecycle_binding");
  hvf::Binding Binding(Host, *Memory);
  ASSERT_EQ(llvm::toString(Binding.execute(
                control(),
                [](auto &Native) -> llvm::Error {
                  uint64_t Time = 0;
                  if (auto S = hv_vcpu_get_exec_time(Native.cpu(), &Time))
                    return hvf::error("lifecycle execution time", S);
                  EXPECT_EQ(Time, 0u);
                  return llvm::Error::success();
                })),
            "");
  probePhase("lifecycle_retirement");
}

TEST_F(HvfIntelProbe, InstructionOnly) {
  probePhase("startup_probe");
  auto Created = createHvfX64Machine(*Memory);
  ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
  auto Machine = std::move(*Created);
  constexpr uint64_t PC = 0x10000;
  const uint8_t Nop[] = {0x90};
  ASSERT_EQ(llvm::toString(Memory->map(PC, 4096, Read | Write | Execute)), "");
  ASSERT_EQ(llvm::toString(Memory->write(PC, Nop)), "");
  ASSERT_EQ(llvm::toString(Memory->beginRun()), "");
  auto Release = llvm::scope_exit([&] { Memory->endRun(); });
  auto Root = buildX64PageTables(*Memory);
  ASSERT_TRUE(bool(Root)) << llvm::toString(Root.takeError());
  X64MachineState State;
  State.reg(X64Register::FLAGS) = x64::InitialFlags;
  State.reg(X64Register::AX) = mach_absolute_time();
  const auto Nonce = State.reg(X64Register::AX);
  probePhase("ordinary_steps");
  for (unsigned I = 0; I < 32; ++I) {
    State.reg(X64Register::PC) = PC;
    ASSERT_EQ(llvm::toString(Machine->step(State, *Root, control())), "");
    ASSERT_EQ(State.reg(X64Register::PC), PC + 1);
    ASSERT_EQ(State.reg(X64Register::AX), Nonce);
    ASSERT_EQ(State.reg(X64Register::FLAGS), x64::InitialFlags);
  }
  probePhase("instruction_retirement");
}

TEST_F(HvfIntelProbe, FiniteDeadline) {
  probePhase("startup_probe");
  auto Created = createHvfX64Machine(*Memory);
  ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
  auto Machine = std::move(*Created);
  constexpr uint64_t PC = 0x10000;
  // nop; mov [rbx],rax; jmp .; nop; inc rax. Preparation executes only NOP.
  const uint8_t Code[] = {0x90, 0x48, 0x89, 0x03, 0xeb,
                          0xfe, 0x90, 0x48, 0xff, 0xc0};
  ASSERT_EQ(llvm::toString(Memory->map(PC, 4096, Read | Write | Execute)), "");
  ASSERT_EQ(llvm::toString(Memory->write(PC, Code)), "");
  auto *Scratch =
      Memory->physicalPointer(Memory->mappings().at(PC).Physical) + 0x100;
  static_assert(std::atomic<uint64_t>::is_always_lock_free);
  auto *Witness = std::construct_at(
      reinterpret_cast<std::atomic<uint64_t> *>(Scratch), uint64_t(0));
  auto DestroyWitness = llvm::scope_exit([&] { std::destroy_at(Witness); });
  ASSERT_EQ(llvm::toString(Memory->beginRun()), "");
  auto Release = llvm::scope_exit([&] { Memory->endRun(); });
  auto Root = buildX64PageTables(*Memory);
  ASSERT_TRUE(bool(Root)) << llvm::toString(Root.takeError());
  X64MachineState State;
  State.reg(X64Register::FLAGS) = x64::InitialFlags;
  State.reg(X64Register::BX) = PC + 0x100;
  auto Prepare = [&](uint64_t Entry) {
    probePhase("prepare_with_transport");
    State.reg(X64Register::AX) = mach_absolute_time();
    State.reg(X64Register::PC) = Entry;
    return Machine->step(State, *Root, control());
  };
  ASSERT_EQ(llvm::toString(Prepare(PC)), "");
  ASSERT_EQ(State.reg(X64Register::PC), PC + 1);
  Witness->store(0); // Only the finite call may establish this witness.
  mach_timebase_info_data_t Timebase{};
  ASSERT_EQ(mach_timebase_info(&Timebase), KERN_SUCCESS);
  ASSERT_NE(Timebase.numer, 0u);
  const uint64_t Slice = (5000000ull * Timebase.denom) / Timebase.numer;
  ASSERT_NE(Slice, 0u);
  hvf::Binding Binding(Host, *Memory);
  uint64_t Reason = 0, RIP = 0, AX = 0;
  unsigned CallIndex = 0;
  auto Call = [&](auto &Native, const char *Phase,
                  uint64_t Deadline) -> llvm::Error {
    uint64_t BeforeExec = 0, AfterExec = 0, BeforeReason = 0, BeforeRIP = 0;
    if (auto S = hv_vcpu_get_exec_time(Native.cpu(), &BeforeExec))
      return hvf::error("probe execution time before", S);
    if (auto S = hv_vmx_vcpu_read_vmcs(Native.cpu(), VMCS_RO_EXIT_REASON,
                                       &BeforeReason))
      return hvf::error("probe reason before", S);
    if (auto S =
            hv_vmx_vcpu_read_vmcs(Native.cpu(), VMCS_GUEST_RIP, &BeforeRIP))
      return hvf::error("probe RIP before", S);
    const auto Index = ++CallIndex;
    llvm::outs() << "INTEL_PROBE begin=" << Phase << " call=" << Index
                 << " deadline=" << Deadline << " rip=" << BeforeRIP
                 << " nonce=" << State.reg(X64Register::AX)
                 << " witness=" << Witness->load() << '\n';
    llvm::outs().flush();
    const auto Before = mach_absolute_time();
    // No Native.run(): isolate the finite API from asynchronous cancellation.
    // The outer diagnostic controller bounds and retires the whole process.
    const auto Status = hv_vcpu_run_until(Native.cpu(), Deadline);
    const auto After = mach_absolute_time();
    llvm::outs() << "INTEL_PROBE end=" << Phase << " call=" << Index
                 << " status=" << uint32_t(Status) << " mach_before=" << Before
                 << " mach_after=" << After << '\n';
    llvm::outs().flush();
    if (Status)
      return hvf::error("finite deadline experiment (no fallback)", Status);
    if (auto S = hv_vcpu_get_exec_time(Native.cpu(), &AfterExec))
      return hvf::error("probe execution time after", S);
    if (auto S =
            hv_vmx_vcpu_read_vmcs(Native.cpu(), VMCS_RO_EXIT_REASON, &Reason))
      return hvf::error("probe exit reason", S);
    if (auto S = hv_vmx_vcpu_read_vmcs(Native.cpu(), VMCS_GUEST_RIP, &RIP))
      return hvf::error("probe RIP", S);
    if (auto S = hv_vcpu_read_register(Native.cpu(), HV_X86_RAX, &AX))
      return hvf::error("probe RAX", S);
    llvm::outs() << "INTEL_PROBE capture=" << Phase << " call=" << Index
                 << " exec_before=" << BeforeExec << " exec_after=" << AfterExec
                 << " reason_before=" << BeforeReason << " reason=" << Reason
                 << " rip_before=" << BeforeRIP << " rip=" << RIP
                 << " rax=" << AX << " nonce=" << State.reg(X64Register::AX)
                 << " witness=" << Witness->load() << '\n';
    llvm::outs().flush();
    return llvm::Error::success();
  };
  const auto WitnessControl = control();
  ASSERT_EQ(
      llvm::toString(Binding.execute(
          WitnessControl,
          [&](auto &Native) -> llvm::Error {
            uint64_t Controls = 0;
            if (auto S = hv_vmx_vcpu_read_vmcs(Native.cpu(),
                                               VMCS_CTRL_CPU_BASED, &Controls))
              return hvf::error("probe controls", S);
            if (auto S =
                    hv_vmx_vcpu_write_vmcs(Native.cpu(), VMCS_CTRL_CPU_BASED,
                                           Controls & ~uint64_t(CPU_BASED_MTF)))
              return hvf::error("probe disable MTF", S);
            if (auto S = hv_vmx_vcpu_write_vmcs(Native.cpu(), VMCS_GUEST_RIP,
                                                PC + 1))
              return hvf::error("probe fresh store PC", S);
            // A finite slice can expire before any guest instruction
            // executes. Preserve that observation and the same guest
            // state; never renew the overall witness deadline.
            const auto MachStart = mach_absolute_time();
            const auto Remaining =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    WitnessControl.Deadline - Clock::now())
                    .count();
            if (Remaining <= 0)
              return diagnostic::error("finite witness budget exhausted");
            const auto End = MachStart + uint64_t(Remaining) * Timebase.denom /
                                             Timebase.numer;
            llvm::outs() << "INTEL_PROBE witness_budget mach_start=" << MachStart
                         << " mach_end=" << End << " remaining_ns=" << Remaining
                         << " timebase_numer=" << Timebase.numer
                         << " timebase_denom=" << Timebase.denom
                         << " max_calls=4096\n";
            llvm::outs().flush();
            auto Deadline = std::min(End, mach_absolute_time() + Slice);
            for (unsigned N = 0; N < 4096; ++N) {
              if (WitnessControl.interrupted())
                return diagnostic::error("finite witness budget exhausted");
              if (auto E = Call(Native, "finite_loop", Deadline))
                return E;
              if (Reason != VMX_REASON_IRQ &&
                  Reason != VMX_REASON_VMX_TIMER_EXPIRED)
                return diagnostic::error("unexpected finite loop exit");
              const auto Nonce = State.reg(X64Register::AX);
              if (AX != Nonce || !((RIP == PC + 1 && Witness->load() == 0) ||
                                   (RIP == PC + 4 && Witness->load() == Nonce)))
                return diagnostic::error("invalid finite guest witness");
              if (WitnessControl.interrupted())
                return diagnostic::error("finite witness budget exhausted");
              if (Reason == VMX_REASON_VMX_TIMER_EXPIRED) {
                if (Witness->load() == Nonce)
                  return llvm::Error::success();
                probePhase("finite_slice_without_guest_progress");
                Deadline = std::min(End, mach_absolute_time() + Slice);
              }
            }
            return diagnostic::error("too many finite deadline exits");
          })),
      "");
  ASSERT_EQ(Witness->load(), State.reg(X64Register::AX));
  ASSERT_EQ(RIP, PC + 4);
  // Observe expiration/entry races with a distinct instruction and old MTF
  // reason. Keep no-entry and one-entry outcomes as distinct observations.
  for (bool Expired : {true, false}) {
    ASSERT_EQ(llvm::toString(Prepare(PC + 6)), "");
    ASSERT_EQ(State.reg(X64Register::PC), PC + 7);
    Witness->store(0);
    ASSERT_EQ(llvm::toString(Binding.execute(
                  control(),
                  [&](auto &Native) -> llvm::Error {
                    return Call(Native, Expired ? "expired_mtf" : "finite_mtf",
                                Expired ? 0 : mach_absolute_time() + Slice);
                  })),
              "");
    // Preserve a stale MTF/no-entry observation, but never classify a guest
    // exception, VM-entry failure or other exit as successful expiration.
    EXPECT_TRUE(Reason == VMX_REASON_MTF || Reason == VMX_REASON_IRQ ||
                Reason == VMX_REASON_VMX_TIMER_EXPIRED)
        << "unexpected MTF probe exit=" << Reason;
    EXPECT_TRUE((RIP == PC + 7 && AX == State.reg(X64Register::AX)) ||
                (RIP == PC + 10 && AX == State.reg(X64Register::AX) + 1));
  }
  probePhase("verified_retry");
  ASSERT_EQ(llvm::toString(Prepare(PC + 6)), "");
  EXPECT_EQ(State.reg(X64Register::PC), PC + 7);
  EXPECT_EQ(State.reg(X64Register::FLAGS), x64::InitialFlags);
  probePhase("finite_retirement");
}

void HvfExecutor::checkIntelRecovery(bool WithHostKick) {
  auto Created = createHvfX64Machine(*Memory);
  ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
  auto Machine = std::move(*Created);
  constexpr uint64_t PC = 0x10000, RetryPC = PC + 2;
  // Keep the loop and retry code immutable. The normal adapter prepares one
  // instruction; only this raw-executor test disables MTF for the busy loop.
  const uint8_t Program[] = {0xeb, 0xfe, 0x90, 0x90}; // jmp .; nop; nop
  ASSERT_EQ(llvm::toString(Memory->map(PC, 4096, Read | Write | Execute)), "");
  ASSERT_EQ(llvm::toString(Memory->write(PC, Program)), "");
  ASSERT_EQ(llvm::toString(Memory->beginRun()), "");
  auto Release = llvm::scope_exit([&] { Memory->endRun(); });
  auto Root = buildX64PageTables(*Memory);
  ASSERT_TRUE(bool(Root)) << llvm::toString(Root.takeError());
  X64MachineState State;
  State.reg(X64Register::FLAGS) = x64::InitialFlags;
  State.reg(X64Register::AX) = 0x12345678;
  auto Prepare = [&](uint64_t Entry) {
    State.reg(X64Register::PC) = Entry;
    return Machine->step(State, *Root, control());
  };
  hvf::Binding Binding(Host, *Memory);
  enum Mode { Deadline, StopToken, HostInterrupt };
  for (auto Kind : {Deadline, StopToken, HostInterrupt}) {
    SCOPED_TRACE(Kind);
    ASSERT_EQ(llvm::toString(Prepare(PC)), "");
    ASSERT_EQ(State.reg(X64Register::PC), PC);
    ASSERT_EQ(State.reg(X64Register::FLAGS), x64::InitialFlags);
    std::atomic<bool> Stop{false};
    std::promise<void> Entered;
    auto Ready = Entered.get_future();
    std::optional<hvf::Cpu> InterruptCPU;
    std::thread Stopper;
    bool HostKickOmitted = false;
    if (Kind != Deadline)
      Stopper = std::thread([&] {
        Ready.wait();
        if (Kind == HostInterrupt && InterruptCPU) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          // An unsolicited host kick cannot complete or fail the busy guest.
          // The same entry must continue until the later requested stop.
          if (WithHostKick)
            EXPECT_EQ(hv_vcpu_interrupt(&*InterruptCPU, 1), HV_SUCCESS);
          else
            HostKickOmitted = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Stop = true;
      });
    auto Control = control();
    Control.Stop = &Stop;
    bool NativeReturned = false;
    const char *Phase = "admission";
    int64_t EntryBudgetMicroseconds = 0;
    auto E = Binding.execute(Control, [&](auto &Native) -> llvm::Error {
      Phase = "VMCS preparation";
      auto Signal = llvm::scope_exit([&] { Entered.set_value(); });
      uint64_t Controls = 0;
      if (auto S = hv_vmx_vcpu_read_vmcs(Native.cpu(), VMCS_CTRL_CPU_BASED,
                                         &Controls))
        return hvf::error("test read stepping controls", S);
      if (auto S = hv_vmx_vcpu_write_vmcs(Native.cpu(), VMCS_CTRL_CPU_BASED,
                                          Controls & ~uint64_t(CPU_BASED_MTF)))
        return hvf::error("test disable MTF", S);
      InterruptCPU = Native.cpu();
      Signal.release();
      Entered.set_value();
      // Admission can spend more than 50 ms waiting or binding memory. This
      // fixture requires native cancellation, so start its short deadline on
      // the admitted owner while preserving the outer cooperative deadline.
      auto NativeControl = Control;
      if (Kind == Deadline)
        NativeControl.Deadline =
            std::min(Control.Deadline,
                     Clock::now() + std::chrono::milliseconds(50));
      Phase = "native entry";
      EntryBudgetMicroseconds =
          std::chrono::duration_cast<std::chrono::microseconds>(
              NativeControl.Deadline - Clock::now())
              .count();
      return Native.run(NativeControl, [&](bool Cancelled) -> llvm::Error {
        NativeReturned = true;
        EXPECT_TRUE(Cancelled);
        uint64_t Reason = 0;
        if (auto S = hv_vmx_vcpu_read_vmcs(Native.cpu(), VMCS_RO_EXIT_REASON,
                                           &Reason))
          return hvf::error("test read interrupted exit", S);
        EXPECT_TRUE(Reason == VMX_REASON_IRQ ||
                    Reason == VMX_REASON_VMX_TIMER_EXPIRED)
            << "reason=" << Reason;
        return llvm::Error::success();
      });
    });
    // An admission failure must also release the helper thread.
    if (Ready.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready)
      Entered.set_value();
    if (Stopper.joinable())
      Stopper.join();
    if (!WithHostKick && Kind == HostInterrupt) {
      EXPECT_TRUE(HostKickOmitted);
      // Emit after joining, outside the timed helper/native cancellation path.
      if (HostKickOmitted) {
        llvm::outs() << "INTEL_RECOVERY host_kick=omitted\n";
        llvm::outs().flush();
      }
    }
    const bool Interrupted = E.isA<MachineInterruptedError>();
    const auto Failure = llvm::toString(std::move(E));
    EXPECT_TRUE(NativeReturned)
        << "phase=" << Phase << "; entry_budget_us=" << EntryBudgetMicroseconds
        << "; interrupted=" << Interrupted << "; error=" << Failure;
    EXPECT_TRUE(Interrupted) << Failure;
    ASSERT_EQ(llvm::toString(Prepare(RetryPC)), "");
    EXPECT_EQ(State.reg(X64Register::PC), RetryPC + 1);
    EXPECT_EQ(State.reg(X64Register::FLAGS), x64::InitialFlags);
    EXPECT_EQ(State.reg(X64Register::AX), 0x12345678u);
  }
  std::atomic<bool> Stop{false};
  auto Control = control();
  Control.Stop = &Stop;
  auto E = Binding.execute(Control, [&](auto &Native) -> llvm::Error {
    return Native.run(Control, [&](bool Cancelled) {
      EXPECT_FALSE(Cancelled);
      Stop = true;
      return diagnostic::error("injected Intel complete-state capture failure");
    });
  });
  EXPECT_FALSE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)),
            "injected Intel complete-state capture failure");
  ASSERT_EQ(llvm::toString(Prepare(RetryPC)), "");
  EXPECT_EQ(State.reg(X64Register::PC), RetryPC + 1);
  EXPECT_EQ(State.reg(X64Register::AX), 0x12345678u);
}

TEST_F(HvfExecutor, NativeIntelCancellationAndCompletionFailureAllowRetry) {
  checkIntelRecovery(true);
}

class HvfIntelRecoveryProbe : public HvfExecutor {
  void SetUp() override {
    if (!probeFlag("NEVERD_HVF_INTEL_PROBE"))
      GTEST_SKIP() << "explicit Intel diagnostic experiment required";
    // Validate the exact lifecycle control before acquiring any native Host.
    ASSERT_TRUE(probeFlag("NEVERD_HVF_INTEL_REUSE_EXECUTOR"));
    ASSERT_TRUE(probeFlag("NEVERD_HVF_INTEL_RECREATE_VCPU"));
    ASSERT_TRUE(probeFlag("NEVERD_HVF_INTEL_RECREATE_VM"));
    ASSERT_FALSE(probeFlag("NEVERD_HVF_INTEL_RECREATE_OWNER"));
    HvfExecutor::SetUp();
  }
};

TEST_F(HvfIntelRecoveryProbe, WithoutHostKick) {
  checkIntelRecovery(false);
}

TEST_F(HvfExecutor, IntelGuestStoreWitnessStopsAndRetries) {
  auto Created = createHvfX64Machine(*Memory);
  ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
  auto Machine = std::move(*Created);
  constexpr uint64_t PC = 0x10000, LoopPC = PC + 4, RetryPC = PC + 6;
  // Preparation executes NOP. Only the subsequent raw entry can store a fresh
  // nonce and reach the busy loop: a timer return alone is not a witness.
  const uint8_t Code[] = {0x90, 0x48, 0x89, 0x03, 0xeb, 0xfe, 0x90};
  ASSERT_EQ(llvm::toString(Memory->map(PC, 4096, Read | Write | Execute)), "");
  ASSERT_EQ(llvm::toString(Memory->write(PC, Code)), "");
  auto *Scratch =
      Memory->physicalPointer(Memory->mappings().at(PC).Physical) + 0x100;
  static_assert(std::atomic<uint64_t>::is_always_lock_free);
  auto *Witness = std::construct_at(
      reinterpret_cast<std::atomic<uint64_t> *>(Scratch), uint64_t(0));
  auto DestroyWitness = llvm::scope_exit([&] { std::destroy_at(Witness); });
  ASSERT_EQ(llvm::toString(Memory->beginRun()), "");
  auto Release = llvm::scope_exit([&] { Memory->endRun(); });
  auto Root = buildX64PageTables(*Memory);
  ASSERT_TRUE(bool(Root)) << llvm::toString(Root.takeError());
  X64MachineState State;
  State.reg(X64Register::FLAGS) = x64::InitialFlags;
  State.reg(X64Register::BX) = PC + 0x100;
  State.reg(X64Register::AX) = mach_absolute_time() | 1;
  State.reg(X64Register::PC) = PC;
  ASSERT_EQ(llvm::toString(Machine->step(State, *Root, control())), "");
  ASSERT_EQ(State.reg(X64Register::PC), PC + 1);
  const auto Nonce = State.reg(X64Register::AX);
  ASSERT_EQ(Witness->load(), 0u);
  std::atomic<bool> Stop{false}, Done{false};
  std::thread Stopper([&] {
    // Do not acquire the core lease from this thread while native execution
    // owns it. The aligned hardware store is the only stop trigger.
    while (Witness->load() != Nonce && !Done.load())
      std::this_thread::yield();
    if (Witness->load() == Nonce)
      Stop = true;
  });
  auto Control = control();
  Control.Stop = &Stop;
  hvf::Binding Binding(Host, *Memory);
  bool NativeReturned = false;
  auto E = Binding.execute(Control, [&](auto &Native) -> llvm::Error {
    uint64_t Controls = 0;
    if (auto S =
            hv_vmx_vcpu_read_vmcs(Native.cpu(), VMCS_CTRL_CPU_BASED, &Controls))
      return hvf::error("witness read stepping controls", S);
    if (auto S = hv_vmx_vcpu_write_vmcs(Native.cpu(), VMCS_CTRL_CPU_BASED,
                                        Controls & ~uint64_t(CPU_BASED_MTF)))
      return hvf::error("witness disable MTF", S);
    return Native.run(Control, [&](bool Cancelled) -> llvm::Error {
      NativeReturned = true;
      EXPECT_TRUE(Cancelled);
      EXPECT_TRUE(Stop.load());
      EXPECT_EQ(Witness->load(), Nonce);
      uint64_t Reason = 0, RIP = 0;
      if (auto S =
              hv_vmx_vcpu_read_vmcs(Native.cpu(), VMCS_RO_EXIT_REASON, &Reason))
        return hvf::error("witness read interrupted exit", S);
      EXPECT_TRUE(Reason == VMX_REASON_IRQ ||
                  Reason == VMX_REASON_VMX_TIMER_EXPIRED);
      if (auto S = hv_vcpu_read_register(Native.cpu(), HV_X86_RIP, &RIP))
        return hvf::error("witness read loop PC", S);
      EXPECT_EQ(RIP, LoopPC);
      return llvm::Error::success();
    });
  });
  Done = true;
  Stopper.join();
  EXPECT_TRUE(NativeReturned);
  EXPECT_TRUE(Stop.load());
  EXPECT_EQ(Witness->load(), Nonce);
  EXPECT_TRUE(E.isA<MachineInterruptedError>());
  llvm::consumeError(std::move(E));
  State.reg(X64Register::PC) = RetryPC;
  ASSERT_EQ(llvm::toString(Machine->step(State, *Root, control())), "");
  EXPECT_EQ(State.reg(X64Register::PC), RetryPC + 1);
  EXPECT_EQ(State.reg(X64Register::AX), Nonce);
  EXPECT_EQ(State.reg(X64Register::FLAGS), x64::InitialFlags);
}
#endif
} // namespace
} // namespace neverd::emulation
#endif
