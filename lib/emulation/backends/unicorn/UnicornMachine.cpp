//===- UnicornMachine.cpp - Portable machine execution -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/aarch64/AArch64State.h"
#include "../../arch/x86_64/X64Exception.h"
#include "../../arch/x86_64/X64Machine.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../MachineFactories.h"
#include "UnicornArchitecture.h"
#include "UnicornMemory.h"

#include "llvm/ADT/ScopeExit.h"

#include <limits>
#include <unicorn/arm64.h>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>

namespace neverd::emulation {
namespace {
llvm::Error check(uc_err Status) {
  if (Status == UC_ERR_OK)
    return llvm::Error::success();
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 uc_strerror(Status));
}
/// The architecture owns admission and observations. This transport executes
/// checked steps or bounded direct runs over the same authoritative RAM pages.
class UnicornStepper {
public:
  uc_engine *Engine = nullptr;
  MemoryProjection &Memory;
  const bool UserMode;
  explicit UnicornStepper(MemoryProjection &Memory, bool UserMode)
      : Memory(Memory), UserMode(UserMode) {}
  ~UnicornStepper() {
    if (Engine)
      uc_close(Engine);
  }
  llvm::Error initialize(uc_arch Arch, uc_mode Mode) {
    if (auto E = check(uc_open(Arch, Mode, &Engine)))
      return E;
    if (auto E = check(
            uc_ctl_tlb_mode(Engine, UserMode ? UC_TLB_CPU : UC_TLB_VIRTUAL)))
      return E;
    if (UserMode)
      for (const auto &Range : Memory.registrations())
        if (auto E = check(uc_mem_map_ptr(Engine, Range.Physical, Range.Size,
                                          UC_PROT_ALL, Range.Backing)))
          return E;
    if (auto E = initializeUnicornArchitecture(
            Engine, Arch == UC_ARCH_X86 ? GuestArchitecture::X64
                                        : GuestArchitecture::AArch64))
      return E;
    uc_hook Hook;
    return check(uc_hook_add(Engine, &Hook, UC_HOOK_BLOCK,
                             reinterpret_cast<void *>(entry), this, 1, 0));
  }
  llvm::Error synchronize() {
    if (UserMode)
      return llvm::Error::success();
    if (MappedSpace.lock() == Memory.addressSpace() &&
        Generation == Memory.mappingGeneration() &&
        DeviceOperand == Memory.deviceOperand())
      return llvm::Error::success();
    if (auto E = forEachUnicornRAMRange(
            Mapped, [&](uint64_t Address, uint64_t Size, const auto &) {
              return check(uc_mem_unmap(Engine, Address, Size));
            }))
      return E;
    Mapped = Memory.mappings();
    if (const auto Operand = Memory.deviceOperand()) {
      auto &Page = Mapped.at(Operand->first);
      Page.IO.reset();
      Page.Physical = Operand->second;
      Page.Permissions = Read | Write;
    }
    if (auto E = forEachUnicornRAMRange(
            Mapped, [&](uint64_t Address, uint64_t Size, const auto &Page) {
              return check(
                  uc_mem_map_ptr(Engine, Address, Size,
                                 Page.Permissions & GuestAccessPermissions,
                                 Memory.physicalPointer(Page.Physical)));
            }))
      return E;
    Generation = Memory.mappingGeneration();
    MappedSpace = Memory.addressSpace();
    DeviceOperand = Memory.deviceOperand();
    return llvm::Error::success();
  }
  llvm::Error run(uint64_t PC, size_t Count = 1,
                  const MachineRunControl *Control = nullptr,
                  bool FixedAArch64Step = false) {
    ActiveControl = Control;
    Cancelled = false;
    auto Release = llvm::scope_exit([&] { ActiveControl = nullptr; });
    if (interrupted()) {
      Cancelled = true;
      return diagnostic::interrupted(diagnostic::UnicornRun, *Control);
    }
    // Host backing writes bypass Unicorn's code-write invalidation. A checked
    // step can execute only the instruction at PC: invalidating its first
    // byte retires every block covering that entry, including an instruction
    // whose operands cross into another physical page. A fixed-width checked
    // ARM64 step may retain that block only after revalidating all its bytes.
    // Direct runs and multi-instruction maintenance can visit other cached
    // entries and still need a full flush. Avoid wrapping the exclusive end.
    auto Bytes = checkedBytes(PC, Count, FixedAArch64Step);
    auto &Cached = Instructions[(PC >> 2) % Instructions.size()];
    const bool Reuse =
        Bytes && Cached.Valid && Cached.PC == PC && Cached.Bytes == *Bytes;
    bool Completed = false;
    auto RetireOnFailure = llvm::scope_exit([&] {
      if (!Completed)
        clearInstructions();
    });
    if (!Reuse) {
      const auto CacheStatus =
          Count == 1 && PC != std::numeric_limits<uint64_t>::max()
              ? uc_ctl_remove_cache(Engine, PC, PC + 1)
              : uc_ctl_flush_tb(Engine);
      if (auto E = check(CacheStatus))
        return E;
    }
    if (interrupted()) {
      Cancelled = true;
      return diagnostic::interrupted(diagnostic::UnicornRun, *Control);
    }
    const auto Status = uc_emu_start(Engine, PC, 0, 0, Count);
    if (Cancelled)
      return diagnostic::interrupted(diagnostic::UnicornRun, *Control);
    if (Status != UC_ERR_OK)
      return check(Status);
    if (interrupted())
      return diagnostic::interrupted(diagnostic::UnicornRun, *Control);
    if (Bytes)
      Cached = {PC, *Bytes, true};
    Completed = true;
    return llvm::Error::success();
  }
  bool entryCancelled() const { return Cancelled; }

private:
  // The pinned Unicorn bounded-step mode translates exactly one instruction.
  // Only aligned AArch64 instructions have this fixed four-byte footprint.
  // Re-read authoritative executable RAM under the checked execution lease;
  // a host/guest alias write cannot be hidden by an unchanged mapping epoch.
  // This is transport reuse, never instruction admission or decoded semantics.
  using InstructionBytes = std::array<uint8_t, 4>;
  struct Instruction {
    uint64_t PC = 0;
    InstructionBytes Bytes{};
    bool Valid = false;
  };
  std::array<Instruction, 2048> Instructions{};
  std::weak_ptr<AddressSpace> InstructionSpace;
  uint64_t InstructionGeneration = 0, InstructionTransport = 0;
  bool InstructionsActive = false;
  void clearInstructions() {
    if (!InstructionsActive)
      return;
    Instructions.fill({});
    InstructionSpace.reset();
    InstructionsActive = false;
  }
  std::optional<InstructionBytes> checkedBytes(uint64_t PC, size_t Count,
                                               bool FixedAArch64Step) {
    // Parallel execution can release the physical lease during transport.
    // Maintenance/direct runs and staged device operands keep full retirement.
    if (!FixedAArch64Step || Count != 1 || (PC & 3) ||
        PC > std::numeric_limits<uint64_t>::max() - 3 ||
        Memory.parallelEnabled() || Memory.deviceOperand()) {
      clearInstructions();
      return std::nullopt;
    }
    if (InstructionSpace.lock() != Memory.addressSpace() ||
        InstructionGeneration != Memory.mappingGeneration() ||
        InstructionTransport != Memory.transportPhysical(0)) {
      clearInstructions();
      InstructionSpace = Memory.addressSpace();
      InstructionGeneration = Memory.mappingGeneration();
      InstructionTransport = Memory.transportPhysical(0);
    }
    InstructionsActive = true;
    InstructionBytes Bytes;
    if (auto E = Memory.read(PC, Bytes, Execute)) {
      // Admission and transport still own the failure and its original PC.
      llvm::consumeError(std::move(E));
      clearInstructions();
      return std::nullopt;
    }
    return Bytes;
  }
  // The synchronous engine call owns this borrow. Checked entries use a
  // one-instruction block; direct entries check at every translated block.
  const MachineRunControl *ActiveControl = nullptr;
  bool Cancelled = false;
  bool interrupted() const {
    return ActiveControl && ActiveControl->interrupted();
  }
  static void entry(uc_engine *Engine, uint64_t, uint32_t, void *UserData) {
    auto &CPU = *static_cast<UnicornStepper *>(UserData);
    if (CPU.interrupted()) {
      CPU.Cancelled = true;
      uc_emu_stop(Engine);
    }
  }
  uint64_t Generation = 0;
  std::weak_ptr<AddressSpace> MappedSpace;
  std::map<uint64_t, MemoryProjection::Page> Mapped;
  std::optional<std::pair<uint64_t, uint64_t>> DeviceOperand;
};
class UnicornX64Machine final : public X64Machine {
public:
  uint32_t mxcsrMask() const override {
    return x64::fp::ArchitecturalMXCSRMask;
  }
  UnicornStepper CPU;
  explicit UnicornX64Machine(MemoryProjection &Memory, bool UserMode)
      : CPU(Memory, UserMode) {}
  llvm::Error initialize() {
    if (auto E = CPU.initialize(UC_ARCH_X86, UC_MODE_64))
      return E;
    uc_hook Hook;
    if (auto E =
            check(uc_hook_add(CPU.Engine, &Hook, UC_HOOK_INTR,
                              reinterpret_cast<void *>(exception), this, 1, 0)))
      return E;
    if (auto E =
            check(uc_hook_add(CPU.Engine, &Hook, UC_HOOK_INSN_INVALID,
                              reinterpret_cast<void *>(invalid), this, 1, 0)))
      return E;
    if (!CPU.UserMode)
      return llvm::Error::success();
    // The portable engine implements these instructions as hook-only helpers,
    // even when EFER.SCE is clear. Turn that transport behavior into the same
    // original-PC exception boundary the architecture receives from hardware.
    for (int Instruction : {UC_X86_INS_SYSCALL, UC_X86_INS_SYSENTER})
      if (auto E = check(uc_hook_add(CPU.Engine, &Hook, UC_HOOK_INSN,
                                     reinterpret_cast<void *>(service), this, 1,
                                     0, Instruction)))
        return E;
    // In 64-bit Unicorn, writing CS/SS changes selectors only. SYSRET installs
    // real CPL3 segment caches before enabling translation. This private boot
    // instruction executes once, before any caller instruction or page table.
    // No monitor virtual address or guest allocation is needed after reset.
    uint64_t CR0 = x64::CR0 & ~x64::Paging, CR4 = x64::CR4;
    uc_x86_msr EFER{x64::EFERAddress, x64::EFER | x64::SystemCallEnable};
    uc_x86_msr STAR{x64::STARAddress, x64::UserSTAR};
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_CR0, &CR0)))
      return E;
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_CR4, &CR4)))
      return E;
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_MSR, &EFER)))
      return E;
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_MSR, &STAR)))
      return E;
    const uint8_t Bootstrap[] = {
#define NEVERD_UNICORN_USER_BOOTSTRAP(...) __VA_ARGS__
#include "UnicornArchitecture.def"
#undef NEVERD_UNICORN_USER_BOOTSTRAP
    };
    if (auto E = check(uc_mem_write(CPU.Engine, x64::BootstrapPC, Bootstrap,
                                    sizeof(Bootstrap))))
      return E;
    uint64_t PC = x64::BootstrapReturnPC, Flags = x64::InitialFlags;
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_RCX, &PC)))
      return E;
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_R11, &Flags)))
      return E;
    if (auto E = CPU.run(x64::BootstrapPC))
      return E;
    if (PendingException)
      return diagnostic::error(x64::exceptiontext::Bootstrap);
    uint64_t ReturnedPC = 0;
    if (auto E = check(uc_reg_read(CPU.Engine, UC_X86_REG_RIP, &ReturnedPC)))
      return E;
    if (ReturnedPC != PC)
      return diagnostic::error(x64::exceptiontext::Bootstrap);
    return llvm::Error::success();
  }
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    return enter(State, Root, Control.forNativeStep(), true);
  }
  llvm::Error run(X64MachineState &State, uint64_t Root,
                  MachineRunControl Control) override {
    if (!CPU.UserMode)
      return diagnostic::error(diagnostic::DirectExecutionUnsupported);
    return enter(State, Root, Control, false);
  }
  bool supportsExecutionStops() const override { return CPU.UserMode; }
  llvm::Error runTo(X64MachineState &State, uint64_t Root,
                    MachineRunControl Control,
                    llvm::ArrayRef<uint64_t> Stops) override {
    if (!CPU.UserMode || Stops.size() > x64::ExecutionStopCount ||
        llvm::any_of(Stops, [](uint64_t PC) { return !x64::canonical(PC); }))
      return diagnostic::error(diagnostic::DirectExecutionUnsupported);
    return enter(State, Root, Control, false, Stops);
  }

private:
  llvm::Error enter(X64MachineState &State, uint64_t Root,
                    MachineRunControl Control, bool Single,
                    llvm::ArrayRef<uint64_t> Stops = {}) {
    if (auto E = validateX64FPState(State.FP))
      return E;
    if (auto E = CPU.synchronize())
      return E;
    if (CPU.UserMode) {
      uint64_t CR0 = x64::CR0;
      if (!Single) {
        // The bootstrap enabled SYSCALL only to install CPL3 through SYSRET.
        // Direct guest services must instead trap at their original PC, just
        // as they do on the native transports; they cannot enter an unset
        // LSTAR or execute a host service.
        uc_x86_msr EFER{x64::EFERAddress, x64::EFER};
        if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_MSR, &EFER)))
          return E;
      }
      if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_CR3, &Root)))
        return E;
      if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_CR0, &CR0)))
        return E;
    }
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  if (auto E = check(uc_reg_write(CPU.Engine, registerID(X64Register::Name),   \
                                  &State.reg(X64Register::Name))))             \
    return E;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    if (auto E =
            check(uc_reg_write(CPU.Engine, UC_X86_REG_GS_BASE, &State.GSBase)))
      return E;
    if (auto E =
            check(uc_reg_write(CPU.Engine, UC_X86_REG_FS_BASE, &State.FSBase)))
      return E;
    for (unsigned I = 0; I < State.Xmm.size(); ++I)
      if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_XMM0 + I,
                                      State.Xmm[I].data())))
        return E;
    if (auto E =
            check(uc_reg_write(CPU.Engine, UC_X86_REG_MXCSR, &State.MXCSR)))
      return E;
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  if (auto E =                                                                 \
          check(uc_reg_write(CPU.Engine, UC_X86_REG_##UC, &State.FP.Member)))  \
    return E;
#include "../../arch/x86_64/X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
    for (unsigned I = 0; I < State.FP.Registers.size(); ++I)
      if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_FP0 + I,
                                      State.FP.Registers[I].data())))
        return E;
    const uint16_t Tag = State.FP.fullTag();
    if (auto E = check(uc_reg_write(CPU.Engine, UC_X86_REG_FPTAG, &Tag)))
      return E;
    PendingException.reset();
    ServicePC.reset();
    ServiceStatus = UC_ERR_OK;
    // Disabling the engine's count hook invalidates old virtual addresses
    // through the current MMU. A watch overlay can make those addresses
    // non-executable, leaking a synthetic fault into the next real exception.
    // Keep the hook installed across checked/direct entries. This transport
    // ceiling is independent of the guest's instruction budget.
    const size_t Count = Single ? 1 : std::numeric_limits<size_t>::max();
    // Address-bounded hooks instrument only these instruction starts, leaving
    // unrelated translated loops free of per-instruction callbacks.
    std::vector<uc_hook> StopHooks;
    auto RemoveStops = [&]() {
      llvm::Error E = llvm::Error::success();
      for (auto Hook : StopHooks)
        E = llvm::joinErrors(std::move(E),
                             check(uc_hook_del(CPU.Engine, Hook)));
      return E;
    };
    for (uint64_t PC : Stops) {
      uc_hook Hook;
      if (auto E = check(uc_hook_add(CPU.Engine, &Hook, UC_HOOK_CODE,
                                     reinterpret_cast<void *>(executionStop),
                                     this, PC, PC)))
        return llvm::joinErrors(std::move(E), RemoveStops());
      StopHooks.push_back(Hook);
    }
    auto RunError = CPU.run(State.reg(X64Register::PC), Count, &Control);
    if (auto E = RemoveStops()) {
      llvm::consumeError(std::move(RunError));
      return E;
    }
    if (ServiceStatus != UC_ERR_OK) {
      llvm::consumeError(std::move(RunError));
      return check(ServiceStatus);
    }
    bool Interrupted = false;
    if (RunError && (!PendingException || CPU.entryCancelled())) {
      if (Single || !RunError.isA<MachineInterruptedError>())
        return RunError;
      // Free execution may already have committed guest work. Capture the
      // boundary it actually stopped on before publishing interruption.
      Interrupted = true;
    }
    if (!Interrupted)
      llvm::consumeError(std::move(RunError));
    auto ConsumeRunError =
        llvm::scope_exit([&] { llvm::consumeError(std::move(RunError)); });
    auto Next = State;
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  if (auto E = check(uc_reg_read(CPU.Engine, registerID(X64Register::Name),    \
                                 &Next.reg(X64Register::Name))))               \
    return E;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    if (auto E =
            check(uc_reg_read(CPU.Engine, UC_X86_REG_GS_BASE, &Next.GSBase)))
      return E;
    if (auto E =
            check(uc_reg_read(CPU.Engine, UC_X86_REG_FS_BASE, &Next.FSBase)))
      return E;
    for (unsigned I = 0; I < Next.Xmm.size(); ++I)
      if (auto E = check(
              uc_reg_read(CPU.Engine, UC_X86_REG_XMM0 + I, Next.Xmm[I].data())))
        return E;
    if (auto E = check(uc_reg_read(CPU.Engine, UC_X86_REG_MXCSR, &Next.MXCSR)))
      return E;
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  if (auto E =                                                                 \
          check(uc_reg_read(CPU.Engine, UC_X86_REG_##UC, &Next.FP.Member)))    \
    return E;
#include "../../arch/x86_64/X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
    for (unsigned I = 0; I < Next.FP.Registers.size(); ++I)
      if (auto E = check(uc_reg_read(CPU.Engine, UC_X86_REG_FP0 + I,
                                     Next.FP.Registers[I].data())))
        return E;
    uint16_t NextTag = 0;
    if (auto E = check(uc_reg_read(CPU.Engine, UC_X86_REG_FPTAG, &NextTag)))
      return E;
    Next.FP.setFullTag(NextTag);
    if (ServicePC)
      Next.reg(X64Register::PC) = *ServicePC;
    if (Interrupted) {
      State = Next;
      return RunError;
    }
    if (PendingException) {
      std::optional<uint64_t> Address;
      if (*PendingException == unsigned(x64::ExceptionVector::PageFault)) {
        uint64_t CR2 = 0;
        if (auto E = check(uc_reg_read(CPU.Engine, UC_X86_REG_CR2, &CR2)))
          return E;
        Address = CR2;
      }
      State = Next;
      return llvm::make_error<X64ExceptionError>(
          X64Exception{*PendingException, std::nullopt, Address});
    }
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::UnicornRun, Control);
    State = Next;
    return llvm::Error::success();
  }

  std::optional<unsigned> PendingException;
  std::optional<uint64_t> ServicePC;
  uc_err ServiceStatus = UC_ERR_OK;
  static void executionStop(uc_engine *Engine, uint64_t, uint32_t, void *) {
    uc_emu_stop(Engine);
  }
  static void service(uc_engine *Engine, void *Opaque) {
    auto &Machine = *static_cast<UnicornX64Machine *>(Opaque);
    uint64_t PC = 0;
    Machine.ServiceStatus = uc_reg_read(Engine, UC_X86_REG_RIP, &PC);
    if (Machine.ServiceStatus == UC_ERR_OK)
      Machine.ServicePC = PC;
    exception(Engine, unsigned(x64::ExceptionVector::InvalidOpcode), Opaque);
  }
  static void exception(uc_engine *Engine, uint32_t Vector, void *Opaque) {
    auto &Machine = *static_cast<UnicornX64Machine *>(Opaque);
    if (!Machine.PendingException)
      Machine.PendingException = Vector;
    uc_emu_stop(Engine);
  }
  static bool invalid(uc_engine *Engine, void *Opaque) {
    exception(Engine, unsigned(x64::ExceptionVector::InvalidOpcode), Opaque);
    return false;
  }
  static int registerID(X64Register R) {
    switch (R) {
#define NEVERD_X64_REGISTER(Name, Decoder, Backend)                            \
  case X64Register::Name:                                                      \
    return Backend;
#include "neverd/emulation/X64Registers.def"
#undef NEVERD_X64_REGISTER
    default:
      return UC_X86_REG_INVALID;
    }
  }
};
/// Borrow a complete register packet for one transport call. IDs and storage
/// pointers come from the same inventory; architectural width normalization
/// and publication remain owned by captureAArch64State.
class AArch64RegisterPacket {
public:
  explicit AArch64RegisterPacket(AArch64MachineState &State) {
    unsigned Next = 0;
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_SCALAR_##Arch(Name)
#define NEVERD_SCALAR_X64(Name)
#define NEVERD_SCALAR_AArch64(Name)                                            \
  Values[Next++] = &State.reg(AArch64Register::Name);
#define NEVERD_VECTOR_REGISTER(Arch, Index, Backend) NEVERD_VECTOR_##Arch(Index)
#define NEVERD_VECTOR_X64(Index)
#define NEVERD_VECTOR_AArch64(Index)                                           \
  Values[Next++] = State.Vectors[Index].data();
#include "neverd/emulation/Registers.def"
#undef NEVERD_VECTOR_AArch64
#undef NEVERD_VECTOR_X64
#undef NEVERD_VECTOR_REGISTER
#undef NEVERD_SCALAR_AArch64
#undef NEVERD_SCALAR_X64
#undef NEVERD_SCALAR_REGISTER
  }
  llvm::Error write(uc_engine *Engine) {
    return check(uc_reg_write_batch(Engine, IDs, Values.data(), Values.size()));
  }
  llvm::Error read(uc_engine *Engine) {
    return check(uc_reg_read_batch(Engine, IDs, Values.data(), Values.size()));
  }

private:
  static constexpr int IDs[] = {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_ID_##Arch(Backend)
#define NEVERD_VECTOR_REGISTER(Arch, Index, Backend) NEVERD_ID_##Arch(Backend)
#define NEVERD_ID_X64(Backend)
#define NEVERD_ID_AArch64(Backend) Backend,
#include "neverd/emulation/Registers.def"
#undef NEVERD_ID_AArch64
#undef NEVERD_ID_X64
#undef NEVERD_VECTOR_REGISTER
#undef NEVERD_SCALAR_REGISTER
  };
  static_assert(std::size(IDs) ==
                std::tuple_size_v<decltype(AArch64MachineState::Registers)> +
                    std::tuple_size_v<decltype(AArch64MachineState::Vectors)>);
  std::array<void *, std::size(IDs)> Values{};
};
class UnicornAArch64Machine final : public AArch64Machine {
public:
  UnicornStepper CPU;
  explicit UnicornAArch64Machine(MemoryProjection &Memory, bool UserMode)
      : CPU(Memory, UserMode) {}
  llvm::Error step(AArch64MachineState &State,
                   MachineRunControl Control) override {
    Control = Control.forNativeStep();
    if (auto E = CPU.synchronize())
      return E;
    const bool UserReady = UserSpace.lock() == CPU.Memory.addressSpace() &&
                           UserGeneration == CPU.Memory.mappingGeneration() &&
                           UserTransportBase == CPU.Memory.transportPhysical(0);
    // A failed step may leave the engine in a partially installed state.
    // Reuse EL0 only after a complete, successful capture on this mapping.
    UserSpace.reset();
    if (CPU.UserMode && !UserReady) {
      // The pinned engine's direct PSTATE write does not rebuild cached EL
      // flags. Configure EL1 before CP_REG writes, then use architectural ERET
      // to enter EL0. Selector/mode metadata alone is not privilege evidence.
      uint64_t Mode = aarch64::PStateEL1h | aarch64::PStateDAIF;
      if (auto E = check(uc_reg_write(CPU.Engine, UC_ARM64_REG_PSTATE, &Mode)))
        return E;
#define NEVERD_AARCH64_SYSTEM_REGISTER(Name, Op0, Op1, CRn, CRm, Op2)          \
  auto Name = [&](uint64_t Value) {                                            \
    uc_arm64_cp_reg R{CRn, CRm, Op0, Op1, Op2, Value};                         \
    return check(uc_reg_write(CPU.Engine, UC_ARM64_REG_CP_REG, &R));           \
  };
#include "../../arch/aarch64/AArch64SystemRegisters.def"
#undef NEVERD_AARCH64_SYSTEM_REGISTER
      if (auto E = Ttbr0El1(aarch64::LowRoot))
        return E;
      if (auto E = Ttbr1El1(aarch64::HighRoot))
        return E;
      if (auto E = TcrEl1(aarch64::TCR))
        return E;
      if (auto E = MairEl1(aarch64::MAIR))
        return E;
      if (auto E = SctlrEl1(aarch64::SCTLR))
        return E;
      if (auto E = VbarEl1(aarch64::VectorGPA))
        return E;
      if (auto E = ElrEl1(State.reg(AArch64Register::PC)))
        return E;
      if (auto E = SpsrEl1(aarch64::PStateEL0t | aarch64::PStateDAIF |
                           State.reg(AArch64Register::NZCV)))
        return E;
      if (auto E = CPU.run(aarch64::EntryGPA,
                           std::size(aarch64::Maintenance) +
                               aarch64::GateReturnInstructions,
                           &Control))
        return E;
    }
    if (auto E = AArch64RegisterPacket(State).write(CPU.Engine))
      return E;
    if (auto E = CPU.run(State.reg(AArch64Register::PC), 1, &Control, true))
      return E;
    // The engine may write only a narrow register or fail after a prefix.
    // Zero the private packet and publish nothing until every read succeeds.
    AArch64MachineState Captured;
    if (auto E = AArch64RegisterPacket(Captured).read(CPU.Engine))
      return E;
    auto Next = State;
    if (auto E = captureAArch64State(
            Next,
            [&](AArch64Register Register) -> llvm::Expected<uint64_t> {
              return Captured.reg(Register);
            },
            [&](unsigned Index) -> llvm::Expected<RegisterValue> {
              return Captured.Vectors[Index];
            }))
      return E;
    uint64_t Mode = 0;
    if (CPU.UserMode) {
      if (auto E = check(uc_reg_read(CPU.Engine, UC_ARM64_REG_PSTATE, &Mode)))
        return E;
    }
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::UnicornRun, Control);
    if (CPU.UserMode &&
        (Mode & aarch64::PStateModeMask) == aarch64::PStateEL0t) {
      UserSpace = CPU.Memory.addressSpace();
      UserGeneration = CPU.Memory.mappingGeneration();
      UserTransportBase = CPU.Memory.transportPhysical(0);
    }
    State = Next;
    return llvm::Error::success();
  }

private:
  std::weak_ptr<AddressSpace> UserSpace;
  uint64_t UserGeneration = 0, UserTransportBase = 0;
};
} // namespace
llvm::Expected<std::unique_ptr<X64Machine>>
createUnicornX64Machine(MemoryProjection &Memory, bool UserMode) {
  auto M = std::make_unique<UnicornX64Machine>(Memory, UserMode);
  if (auto E = M->initialize())
    return E;
  return std::unique_ptr<X64Machine>(std::move(M));
}
llvm::Expected<std::unique_ptr<AArch64Machine>>
createUnicornAArch64Machine(MemoryProjection &Memory, bool UserMode) {
  auto M = std::make_unique<UnicornAArch64Machine>(Memory, UserMode);
  if (auto E = M->CPU.initialize(UC_ARCH_ARM64, UC_MODE_ARM))
    return E;
  return std::unique_ptr<AArch64Machine>(std::move(M));
}
} // namespace neverd::emulation
