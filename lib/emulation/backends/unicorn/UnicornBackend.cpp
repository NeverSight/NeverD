//===- UnicornBackend.cpp - Checked Unicorn CPU adapter -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checked Unicorn CPU adapter.
///
//===----------------------------------------------------------------------===//

#include "UnicornBackend.h"

#include "../../arch/x86_64/X64FPState.h"
#include "../../arch/x86_64/X64Machine.h"
#include "../../core/ExecutionDeadline.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../../core/ExecutionExitBuilder.h"
#include "../../core/MemoryLayout.h"
#include "../../core/MemoryProjection.h"
#include "UnicornAArch64Atomic.h"
#include "UnicornArchitecture.h"
#include "UnicornMemory.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <atomic>
#include <capstone/capstone.h>
#include <exception>
#include <iterator>
#include <map>
#include <new>
#include <unicorn/arm64.h>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#include <utility>

namespace neverd::emulation {
namespace {
namespace unicornDiagnostic {
#define NEVERD_UNICORN_DIAGNOSTIC(Name, Text)                                  \
  inline constexpr char Name[] = Text;
#include "UnicornDiagnostics.def"
#undef NEVERD_UNICORN_DIAGNOSTIC
} // namespace unicornDiagnostic
llvm::Error failure(const std::string &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
llvm::Error check(uc_err Error, const char *Operation) {
  if (Error == UC_ERR_OK)
    return llvm::Error::success();
  return failure(std::string(Operation) +
                 unicornDiagnostic::OperationSeparator + uc_strerror(Error));
}
int registerID(CPURegister Register) {
  switch (Register) {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  case CPURegister::Arch##Name:                                                \
    return Backend;
#define NEVERD_VECTOR_REGISTER(Arch, Index, Backend)                           \
  case CPURegister::Arch##V##Index:                                            \
    return Backend;
#define NEVERD_EXTENDED_REGISTER NEVERD_SCALAR_REGISTER
#include "neverd/emulation/Registers.def"
#undef NEVERD_EXTENDED_REGISTER
#undef NEVERD_SCALAR_REGISTER
#undef NEVERD_VECTOR_REGISTER
  case CPURegister::Invalid:
    break;
  }
  return 0;
}
} // namespace

struct UnicornBackend::Impl {
  struct MMIORegion {
    Impl *Backend;
    std::shared_ptr<MemoryProjection::Device> IO;
  };
  uc_engine *Engine = nullptr;
  GuestArchitecture Architecture = GuestArchitecture::X64;
  int PCRegister = UC_X86_REG_RIP;
  // A separate identity survives address reuse without extending engine life.
  std::shared_ptr<const void> Identity = std::make_shared<unsigned char>(0);
  std::unique_ptr<MemoryProjection> Memory;
  std::map<uint64_t, MemoryProjection::Page> ProjectedRAM;
  std::map<uint64_t, std::unique_ptr<MMIORegion>> MMIO;
  std::weak_ptr<AddressSpace> ProjectedSpace;
  uint64_t Generation = 0;
  BackendHooks Hooks;
  csh Decoder = 0;
  cs_insn *Decoded = nullptr;
  bool HasWriteWatches = false, PendingWriteWatch = false;
  uc_hook WriteWatchHook = 0;
  std::vector<uc_hook> HookHandles;
  std::optional<BackendFault> FirstFault;
  std::optional<BackendFault> RecoverableFault;
  std::shared_ptr<RAMReservation> Exclusive;
  std::string InstructionFailure;
  bool InstructionRejected = false;
  uint64_t InstructionPC = 0;
  bool Timeout = false;
  bool CallbackFailed = false;
  std::atomic<bool> Running{false}, StopRequested{false};
  bool DeviceCallbackActive = false;
  bool MMIOFailed = false;
  std::string MMIOFailure;

  ~Impl() {
    if (Decoded)
      cs_free(Decoded, 1);
    if (Decoder)
      cs_close(&Decoder);
    if (Engine)
      uc_close(Engine);
  }

  std::optional<BackendFaultKind> accessFault(uint64_t Address, uint64_t Size,
                                              unsigned Permission) const {
    return Memory->check(Address, Size, Permission);
  }

  bool accessible(uint64_t Address, uint64_t Size, unsigned Permission) const {
    auto Result = Memory->addressSpace()->canAccess(Address, Size, Permission);
    if (!Result) {
      llvm::consumeError(Result.takeError());
      return false;
    }
    return *Result;
  }

  MemoryProjection::Device *overlappingMMIO(uint64_t Address,
                                            uint64_t Size) const {
    if (!Size)
      return nullptr;
    for (const auto &[Base, Region] : Memory->devices())
      if (Address <= Base ? Base - Address < Size
                          : Address - Base < Region->Size)
        return Region.get();
    return nullptr;
  }

  llvm::Error validateRAMBacking(uint64_t Address, uint64_t Size) const {
    return Memory->addressSpace()->validateBacking(Address, Size);
  }

  llvm::Error mutableState() const {
    if (Running || DeviceCallbackActive)
      return diagnostic::error(diagnostic::Running);
    if (effectsStopped() || RecoverableFault)
      return diagnostic::error(diagnostic::Faulted);
    return Memory->mutableMemory();
  }

  llvm::Error synchronize() {
    if (ProjectedSpace.lock() == Memory->addressSpace() &&
        Generation == Memory->mappingGeneration())
      return llvm::Error::success();
    auto Mutation = [&](uc_err Status, const char *Operation) -> llvm::Error {
      if (Status != UC_ERR_OK)
        retain({BackendFaultKind::UnhandledException, InstructionPC});
      return check(Status, Operation);
    };
    if (auto E = forEachUnicornRAMRange(
            ProjectedRAM, [&](uint64_t Address, uint64_t Size, const auto &) {
              return Mutation(uc_mem_unmap(Engine, Address, Size),
                              unicornDiagnostic::UnmapSharedGuestMemory);
            }))
      return E;
    for (const auto &[Address, Region] : MMIO)
      if (auto E = Mutation(uc_mem_unmap(Engine, Address, Region->IO->Size),
                            unicornDiagnostic::UnmapGuestMMIO))
        return E;
    // All obsolete engine pointers have now retired. Only now release pins.
    ProjectedRAM.clear();
    MMIO.clear();
    for (const auto &[Address, Page] : Memory->mappings())
      if (!Page.IO)
        ProjectedRAM.emplace(Address, Page);
    if (auto E = forEachUnicornRAMRange(
            ProjectedRAM,
            [&](uint64_t Address, uint64_t Size, const auto &Page) {
              return Mutation(
                  uc_mem_map_ptr(Engine, Address, Size,
                                 Page.Permissions & GuestAccessPermissions,
                                 Memory->physicalPointer(Page.Physical)),
                  unicornDiagnostic::MapGuestMemory);
            }))
      return E;
    for (const auto &[Address, Device] : Memory->devices()) {
      auto Region = std::make_unique<MMIORegion>(MMIORegion{this, Device});
      auto *Identity = Region.get();
      MMIO.emplace(Address, std::move(Region));
      if (auto E = Mutation(uc_mmio_map(Engine, Address, Device->Size, mmioRead,
                                        Identity, mmioWrite, Identity),
                            unicornDiagnostic::MapGuestMMIO))
        return E;
    }
    ProjectedSpace = Memory->addressSpace();
    Generation = Memory->mappingGeneration();
    return llvm::Error::success();
  }

  bool effectsStopped() const {
    return MMIOFailed || CallbackFailed || FirstFault ||
           (Running && StopRequested);
  }

  llvm::Error deviceError() const {
    if (MMIOFailed)
      return failure(MMIOFailure.empty() ? unicornDiagnostic::MMIOCallbackFailed
                                         : MMIOFailure);
    if (CallbackFailed)
      return failure(unicornDiagnostic::ExceptionInEmulatorHook);
    return llvm::Error::success();
  }

  void failMMIO(llvm::Error Error) {
    if (!MMIOFailed) {
      MMIOFailed = true;
      MMIOFailure = llvm::toString(std::move(Error));
    } else {
      llvm::consumeError(std::move(Error));
    }
    uc_emu_stop(Engine);
  }

  llvm::Error validateMMIO(uint64_t Address, uint64_t Size, bool IsWrite) {
    auto *Region = overlappingMMIO(Address, Size);
    if (!Region)
      return llvm::Error::success();
    if (Address < Region->Address ||
        Address - Region->Address >= Region->Size ||
        Size > Region->Size - (Address - Region->Address))
      return failure(unicornDiagnostic::MMIOAccessCrossesAMappingBoundary);
    if ((Size != 1 && Size != 2 && Size != 4) || Address % Size)
      return failure(unicornDiagnostic::MMIORequiresAnAligned12Or4);
    if (DeviceCallbackActive)
      return failure(
          unicornDiagnostic::RecursiveMMIOCallbackAccessIsUnsupported);
    DeviceCallbackActive = true;
    auto Reset = llvm::scope_exit([&] { DeviceCallbackActive = false; });
    return Region->Callbacks.Validate(Address - Region->Address, Size, IsWrite);
  }

  void preflightMMIO(uint64_t Address, uint64_t Size, bool IsWrite) {
    if (auto E = validateMMIO(Address, Size, IsWrite))
      failMMIO(std::move(E));
  }

  static uint64_t mmioRead(uc_engine *, uint64_t Offset, unsigned Size,
                           void *Opaque) noexcept {
    auto &Region = *static_cast<MMIORegion *>(Opaque);
    auto &S = *Region.Backend;
    uint64_t Value = 0;
    S.invoke([&] {
      if (S.effectsStopped())
        return;
      S.preflightMMIO(Region.IO->Address + Offset, Size, false);
      if (S.effectsStopped())
        return;
      S.DeviceCallbackActive = true;
      auto Reset = llvm::scope_exit([&] { S.DeviceCallbackActive = false; });
      auto Result = Region.IO->Callbacks.Read(Offset, Size);
      if (!Result) {
        S.failMMIO(Result.takeError());
        return;
      }
      Value = *Result & ((uint64_t(1) << (Size * 8)) - 1);
    });
    return Value;
  }

  static void mmioWrite(uc_engine *, uint64_t Offset, unsigned Size,
                        uint64_t Value, void *Opaque) noexcept {
    auto &Region = *static_cast<MMIORegion *>(Opaque);
    auto &S = *Region.Backend;
    S.invoke([&] {
      if (S.effectsStopped())
        return;
      S.preflightMMIO(Region.IO->Address + Offset, Size, true);
      if (S.effectsStopped())
        return;
      S.DeviceCallbackActive = true;
      auto Reset = llvm::scope_exit([&] { S.DeviceCallbackActive = false; });
      if (auto E = Region.IO->Callbacks.Write(
              Offset, Size, Value & ((uint64_t(1) << (Size * 8)) - 1)))
        S.failMMIO(std::move(E));
    });
  }

  uint64_t currentPC() const noexcept {
    uint64_t PC = InstructionPC;
    // Capture inside the fault boundary, before a hook can change the CPU.
    if (uc_reg_read(Engine, PCRegister, &PC) != UC_ERR_OK)
      return InstructionPC;
    return PC;
  }

  void retain(BackendFault Fault) noexcept {
    if (!FirstFault)
      FirstFault = Fault;
    Exclusive.reset();
  }

  llvm::Error atomicFault(BackendFault Fault) {
    if (Hooks.RecoverableFault && Hooks.RecoverableFault(Fault)) {
      Exclusive.reset();
      RecoverableFault = Fault;
      uc_emu_stop(Engine);
      return llvm::Error::success();
    }
    retain(Fault);
    if (Fault.Address && Fault.Size && Fault.Access && Hooks.Fault)
      Hooks.Fault(*Fault.Address, *Fault.Size,
                  backendAccessKindName(*Fault.Access));
    uc_emu_stop(Engine);
    return diagnostic::error(diagnostic::MemoryAccess);
  }

  void memoryFault(BackendFaultKind Kind, BackendAccessKind Access,
                   uint64_t Address, uint64_t Size) noexcept {
    retain({Kind, currentPC(), Address, Size, Access, std::nullopt});
  }

  template <typename F> void invoke(F &&Function) noexcept {
    try {
      Function();
    } catch (...) {
      // A hook may have failed because allocation itself failed. Keep this
      // noexcept callback free of further allocation; diagnose after Unicorn
      // returns to the ordinary C++ boundary.
      CallbackFailed = true;
      uc_emu_stop(Engine);
    }
  }
  static void code(uc_engine *, uint64_t Address, uint32_t Size, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    S.InstructionPC = Address;
    S.invoke([&] {
      // Only a completed RAM store arms this notification: a pre-store hook
      // can be followed by a self-modifying-code retry at the same PC.
      // Publish at the next instruction boundary, before admitting it. A
      // fault or service request before this boundary keeps exit priority.
      if (std::exchange(S.PendingWriteWatch, false) && S.Hooks.MemoryWritten) {
        S.Hooks.MemoryWritten();
        if (S.effectsStopped() || S.RecoverableFault)
          return;
      }
      if (S.Hooks.Instruction)
        S.Hooks.Instruction(Address, Size);
      if (S.Architecture != GuestArchitecture::AArch64 || S.effectsStopped() ||
          S.RecoverableFault)
        return;
      auto E = executeUnicornAArch64Atomic(
          S.Engine, Address, *S.Memory, S.Exclusive,
          {S.Hooks, [&] { return S.effectsStopped() || S.RecoverableFault; },
           [&](BackendFault Fault) { return S.atomicFault(Fault); },
           [&](uint64_t At, uint64_t Count,
               unsigned Permissions) -> llvm::Error {
             if (auto F = S.Memory->firstAccessFailure(At, Count, Permissions))
               return S.atomicFault({F->Kind, Address, F->Address, F->Size,
                                     Permissions == Read
                                         ? BackendAccessKind::Read
                                         : BackendAccessKind::Write});
             return llvm::Error::success();
           },
           Write, AArch64AtomicAlignment::Natural},
          registerID);
      if (E) {
        S.InstructionRejected = E.isA<UnsupportedExecutionError>();
        if (!S.FirstFault) {
          S.CallbackFailed = !S.InstructionRejected;
          S.retain({S.InstructionRejected
                        ? BackendFaultKind::InvalidInstruction
                        : BackendFaultKind::UnhandledException,
                    Address});
          if (S.InstructionRejected && S.Hooks.InvalidInstruction)
            S.Hooks.InvalidInstruction();
        }
        S.InstructionFailure = llvm::toString(std::move(E));
        uc_emu_stop(S.Engine);
      }
    });
  }
  static void write(uc_engine *, uc_mem_type, uint64_t Address, int Size,
                    int64_t Value, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    S.invoke([&] {
      if (S.effectsStopped())
        return;
      S.preflightMMIO(Address, Size, true);
      if (S.effectsStopped())
        return;
      if (S.Hooks.Write)
        S.Hooks.Write(Address, Size, uint64_t(Value));
      if (S.Architecture == GuestArchitecture::AArch64 && Size > 0 &&
          !S.effectsStopped() && !S.RecoverableFault)
        observeUnicornRAMWrite(*S.Memory, Address, Size);
    });
  }
  static void written(uc_engine *, uc_mem_type, uint64_t Address, int Size,
                      int64_t, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    S.invoke([&] {
      if (S.HasWriteWatches && Size > 0 && !S.effectsStopped() &&
          !S.RecoverableFault) {
        // Resolve aliases through the shared physical mapping authority.
        uint64_t At = Address, Remaining = uint64_t(Size);
        while (Remaining) {
          const uint64_t Offset = At % memory::PageSize;
          const uint64_t Count = std::min(Remaining, memory::PageSize - Offset);
          auto Page = S.Memory->mappings().find(At - Offset);
          if (Page != S.Memory->mappings().end() && !Page->second.IO)
            S.PendingWriteWatch |=
                S.Memory->writeWatched(Page->second.Physical + Offset, Count);
          Remaining -= Count;
          if (Count > UINT64_MAX - At)
            break;
          At += Count;
        }
      }
    });
  }
  static void read(uc_engine *, uc_mem_type, uint64_t Address, int Size,
                   int64_t, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    S.invoke([&] {
      if (S.effectsStopped())
        return;
      S.preflightMMIO(Address, Size, false);
      if (S.effectsStopped())
        return;
      if (S.Hooks.Read)
        S.Hooks.Read(Address, Size);
    });
  }
  static bool fault(uc_engine *, uc_mem_type Type, uint64_t Address, int Size,
                    int64_t, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    BackendAccessKind Access;
    BackendFaultKind Kind;
    switch (Type) {
#define NEVERD_UNICORN_MEMORY_FAULT(Event, FaultKind, AccessKind)              \
  case Event:                                                                  \
    Kind = BackendFaultKind::FaultKind;                                        \
    Access = BackendAccessKind::AccessKind;                                    \
    break;
#include "UnicornFaults.def"
#undef NEVERD_UNICORN_MEMORY_FAULT
    default:
      S.CallbackFailed = true;
      uc_emu_stop(S.Engine);
      return false;
    }
    BackendFault Fault{
        Kind,    S.currentPC(),
        Address, Size > 0 ? std::optional<uint64_t>(Size) : std::nullopt,
        Access,  std::nullopt};
    S.invoke([&] {
      if (!S.effectsStopped() && S.Hooks.RecoverableFault &&
          S.Hooks.RecoverableFault(Fault)) {
        S.Exclusive.reset();
        S.RecoverableFault = Fault;
        uc_emu_stop(S.Engine);
      }
    });
    if (S.RecoverableFault)
      return false;
    S.retain(Fault);
    S.invoke([&] {
      if (S.Hooks.Fault)
        S.Hooks.Fault(Address, Size > 0 ? uint32_t(Size) : 0,
                      backendAccessKindName(Access));
    });
    return false;
  }
  static void interrupt(uc_engine *, uint32_t Number, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    // RIP may already follow INT3. The code hook identifies the instruction
    // that raised the event, including a synchronous CPU exception such as #DE.
    BackendFault Fault{BackendFaultKind::Interrupt,
                       S.InstructionPC,
                       std::nullopt,
                       std::nullopt,
                       std::nullopt,
                       Number};
    S.invoke([&] {
      if (!S.effectsStopped() && S.Hooks.RecoverableFault &&
          S.Hooks.RecoverableFault(Fault))
        S.RecoverableFault = Fault;
    });
    if (S.RecoverableFault) {
      S.Exclusive.reset();
      uc_emu_stop(S.Engine);
      return;
    }
    S.retain(Fault);
    S.invoke([&] {
      if (S.Hooks.Interrupt)
        S.Hooks.Interrupt(Number);
    });
    uc_emu_stop(S.Engine);
  }
  static bool invalidInstruction(uc_engine *, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    S.retain({BackendFaultKind::InvalidInstruction, S.currentPC(), std::nullopt,
              std::nullopt, std::nullopt, std::nullopt});
    S.invoke([&] {
      if (S.Hooks.InvalidInstruction)
        S.Hooks.InvalidInstruction();
    });
    return false;
  }
};

struct UnicornContext final : BackendContext::Storage {
  uc_context *Context = nullptr;
  std::shared_ptr<RAMReservation> Exclusive;

  ~UnicornContext() override {
    if (Context)
      uc_context_free(Context);
  }
};

UnicornBackend::UnicornBackend(std::unique_ptr<Impl> State)
    : State(std::move(State)) {}
UnicornBackend::~UnicornBackend() = default;

llvm::Expected<std::unique_ptr<UnicornBackend>>
UnicornBackend::create(uint64_t MemoryLimit, GuestArchitecture Architecture) {
  auto RAM = PhysicalMemory::create(MemoryLimit);
  if (!RAM)
    return RAM.takeError();
  auto Space = AddressSpace::create(std::move(*RAM), MemoryLimit);
  if (!Space)
    return Space.takeError();
  return create(std::move(*Space), Architecture);
}
llvm::Expected<std::unique_ptr<UnicornBackend>>
UnicornBackend::create(std::shared_ptr<AddressSpace> Space,
                       GuestArchitecture Architecture) {
  auto Memory = MemoryProjection::create(std::move(Space));
  if (!Memory)
    return Memory.takeError();
  auto S = std::make_unique<Impl>();
  S->Memory = std::move(*Memory);
  S->Architecture = Architecture;
  uc_arch Arch;
  uc_mode Mode;
  switch (Architecture) {
  case GuestArchitecture::X64:
    Arch = UC_ARCH_X86;
    Mode = UC_MODE_64;
    break;
  case GuestArchitecture::AArch64:
    Arch = UC_ARCH_ARM64;
    Mode = UC_MODE_ARM;
    S->PCRegister = UC_ARM64_REG_PC;
    break;
  default:
    return diagnostic::error(diagnostic::Architecture);
  }
  if (auto E =
          check(uc_open(Arch, Mode, &S->Engine), diagnostic::UnicornCreate))
    return std::move(E);
  // GuestMemory maps virtual addresses directly without Windows page tables.
  // The CPU TLB applies its physical address width even with paging disabled,
  // which truncates canonical kernel addresses. Unicorn's virtual TLB keeps
  // these addresses intact while retaining the mapped page permissions.
  if (auto E = check(uc_ctl_tlb_mode(S->Engine, UC_TLB_VIRTUAL),
                     unicornDiagnostic::ConfigureGuestVirtualAddressSpace))
    return std::move(E);
  // Scheduling exchanges CPU state while every thread observes the same live
  // address space. Never enable Unicorn's optional memory snapshot mode.
  if (auto E = check(uc_ctl_context_mode(S->Engine, UC_CTL_CONTEXT_CPU),
                     unicornDiagnostic::ConfigureCPUContextContents))
    return std::move(E);
  if (auto E = initializeUnicornArchitecture(S->Engine, Architecture))
    return std::move(E);
  auto Backend =
      std::unique_ptr<UnicornBackend>(new UnicornBackend(std::move(S)));
  // Core fault capture must also work without optional tracing callbacks.
  if (auto E = Backend->installHooks({}))
    return std::move(E);
  return Backend;
}

std::shared_ptr<AddressSpace> UnicornBackend::addressSpace() const {
  return State->Memory->addressSpace();
}
llvm::Error
UnicornBackend::bindAddressSpace(std::shared_ptr<AddressSpace> Space) {
  if (auto E = State->mutableState())
    return E;
  if (auto E = State->Memory->bind(
          std::move(Space), [](uint64_t, uint64_t) { return true; }, true))
    return E;
  State->Exclusive.reset();
  return llvm::Error::success();
}
llvm::Error UnicornBackend::map(uint64_t Address, uint64_t Size,
                                unsigned Permissions) {
  if (auto E = State->mutableState())
    return E;
  // Preserve the legacy RAM-map error category. AddressSpace exposes the
  // typed allocation shortage for new memory clients; MMIO already used it.
  return llvm::handleErrors(
      addressSpace()->map(Address, Size, Permissions),
      [](const GuestMemoryLimitError &) {
        return failure(
            unicornDiagnostic::InvalidGuestMappingOrMemoryLimitExceeded);
      });
}
llvm::Error UnicornBackend::mapAlias(uint64_t Address, uint64_t Source,
                                     uint64_t Size, unsigned Permissions) {
  return replaceAliases({}, {{Address, Source, Size, Permissions}});
}
llvm::Error UnicornBackend::unmapAlias(uint64_t Address, uint64_t Size) {
  return replaceAliases({{Address, Size}}, {});
}
llvm::Error
UnicornBackend::replaceAliases(llvm::ArrayRef<GuestAliasRange> Remove,
                               llvm::ArrayRef<GuestAliasMapping> Add) {
  if (auto E = State->mutableState())
    return E;
  return addressSpace()->replaceAliases(Remove, Add);
}
llvm::Error UnicornBackend::protect(uint64_t Address, uint64_t Size,
                                    unsigned Permissions) {
  if (auto E = State->mutableState())
    return E;
  return addressSpace()->protect(Address, Size, Permissions);
}
llvm::Error UnicornBackend::mapMMIO(uint64_t Address, uint64_t Size,
                                    GuestMMIOCallbacks Callbacks) {
  if (State->Running || State->DeviceCallbackActive)
    return failure(unicornDiagnostic::CannotMapMMIODuringGuestExecutionOrA);
  if (auto E = State->mutableState())
    return E;
  return addressSpace()->mapMMIO(Address, Size, std::move(Callbacks));
}
llvm::Error UnicornBackend::unmapMMIO(uint64_t Address, uint64_t Size) {
  if (State->Running || State->DeviceCallbackActive)
    return failure(unicornDiagnostic::CannotUnmapMMIODuringGuestExecutionOrA);
  if (auto E = State->mutableState())
    return E;
  return addressSpace()->unmapMMIO(Address, Size);
}

llvm::Error UnicornBackend::read(uint64_t Address,
                                 llvm::MutableArrayRef<uint8_t> Bytes) {
  auto Lock = State->Memory->lock();
  if (!Lock)
    return Lock.takeError();
  if (auto E = State->deviceError())
    return E;
  if (auto Kind = State->accessFault(Address, Bytes.size(), Read)) {
    State->memoryFault(*Kind, BackendAccessKind::Read, Address, Bytes.size());
    return failure(std::string(unicornDiagnostic::GuestReadFaultAt0x) +
                   llvm::utohexstr(Address));
  }
  if (!State->overlappingMMIO(Address, Bytes.size()))
    return addressSpace()->read(Address, Bytes);
  if (State->overlappingMMIO(Address, Bytes.size()) && State->effectsStopped())
    return failure(unicornDiagnostic::CannotAccessMMIOOnAStoppedOrFaulted);
  if (State->DeviceCallbackActive) {
    State->invoke([&] { State->preflightMMIO(Address, Bytes.size(), false); });
    return State->deviceError();
  }
  if (auto E = State->Memory->beginRun())
    return E;
  auto Release = llvm::scope_exit([&] { State->Memory->endRun(); });
  if (auto E = State->synchronize())
    return E;
  State->invoke([&] { State->preflightMMIO(Address, Bytes.size(), false); });
  if (auto E = State->deviceError())
    return E;
  auto Status = Bytes.empty() ? UC_ERR_OK
                              : uc_mem_read(State->Engine, Address,
                                            Bytes.data(), Bytes.size());
  if (auto E = State->deviceError())
    return E;
  return check(Status, unicornDiagnostic::ReadGuestMemory);
}
llvm::Error UnicornBackend::write(uint64_t Address,
                                  llvm::ArrayRef<uint8_t> Bytes) {
  auto Lock = State->Memory->lock();
  if (!Lock)
    return Lock.takeError();
  if (auto E = State->deviceError())
    return E;
  if (State->DeviceCallbackActive &&
      State->overlappingMMIO(Address, Bytes.size())) {
    State->invoke([&] { State->preflightMMIO(Address, Bytes.size(), true); });
    return State->deviceError();
  }
  if (State->Running || State->DeviceCallbackActive)
    return diagnostic::error(diagnostic::Running);
  if (auto E = State->Memory->mutableMemory())
    return E;

  if (auto Kind = State->accessFault(Address, Bytes.size(), Write)) {
    State->memoryFault(*Kind, BackendAccessKind::Write, Address, Bytes.size());
    return failure(std::string(unicornDiagnostic::GuestWriteFaultAt0x) +
                   llvm::utohexstr(Address));
  }
  if (!State->overlappingMMIO(Address, Bytes.size()))
    return addressSpace()->write(Address, Bytes);
  if (State->overlappingMMIO(Address, Bytes.size()) && State->effectsStopped())
    return failure(unicornDiagnostic::CannotAccessMMIOOnAStoppedOrFaulted);
  if (auto E = State->Memory->beginRun())
    return E;
  auto Release = llvm::scope_exit([&] { State->Memory->endRun(); });
  if (auto E = State->synchronize())
    return E;
  State->invoke([&] { State->preflightMMIO(Address, Bytes.size(), true); });
  if (auto E = State->deviceError())
    return E;
  auto Status = Bytes.empty() ? UC_ERR_OK
                              : uc_mem_write(State->Engine, Address,
                                             Bytes.data(), Bytes.size());
  if (auto E = State->deviceError())
    return E;
  return check(Status, unicornDiagnostic::WriteGuestMemory);
}
llvm::Error UnicornBackend::fetch(uint64_t Address,
                                  llvm::MutableArrayRef<uint8_t> Bytes) {
  auto Lock = State->Memory->lock();
  if (!Lock)
    return Lock.takeError();
  if (auto Kind = State->accessFault(Address, Bytes.size(), Execute)) {
    State->memoryFault(*Kind, BackendAccessKind::Execute, Address,
                       Bytes.size());
    return failure(std::string(unicornDiagnostic::GuestFetchFaultAt0x) +
                   llvm::utohexstr(Address));
  }
  return State->Memory->read(Address, Bytes, Execute);
}

llvm::Error UnicornBackend::validateBacking(uint64_t Address,
                                            uint64_t Size) const {
  if (State->Running || State->DeviceCallbackActive)
    return failure(
        unicornDiagnostic::RAMBackingAccessRequiresAStoppedCPUWithout);
  if (State->effectsStopped())
    return failure(unicornDiagnostic::CannotAccessRAMBackingOnAFaultedCPU);
  return State->validateRAMBacking(Address, Size);
}

llvm::Error
UnicornBackend::snapshotBacking(uint64_t Address,
                                llvm::MutableArrayRef<uint8_t> Bytes) {
  if (State->Running || State->DeviceCallbackActive)
    return failure(unicornDiagnostic::RAMSnapshotRequiresAStoppedCPUWithoutAn);
  if (auto E = State->validateRAMBacking(Address, Bytes.size()))
    return E;
  return addressSpace()->snapshotBacking(Address, Bytes);
}

llvm::Expected<bool> UnicornBackend::canAccess(uint64_t Address, uint64_t Size,
                                               unsigned Permissions) const {
  if ((Permissions & ~GuestPermissionMask) || State->DeviceCallbackActive ||
      State->effectsStopped())
    return failure(
        unicornDiagnostic::CPUAccessPreflightRequiresValidPermissionsAndA);
  return addressSpace()->canAccess(Address, Size, Permissions);
}

llvm::Error UnicornBackend::readBacking(uint64_t Address,
                                        llvm::MutableArrayRef<uint8_t> Bytes) {
  if (auto E = validateBacking(Address, Bytes.size()))
    return E;
  return addressSpace()->readBacking(Address, Bytes);
}

llvm::Error UnicornBackend::writeBacking(uint64_t Address,
                                         llvm::ArrayRef<uint8_t> Bytes) {
  if (auto E = validateBacking(Address, Bytes.size()))
    return E;
  return addressSpace()->writeBacking(Address, Bytes);
}
GuestArchitecture UnicornBackend::architecture() const {
  return State->Architecture;
}
llvm::Expected<RegisterValue>
UnicornBackend::supportedControlBits(CPURegister R) const {
  if (architecture() == GuestArchitecture::X64 && R == CPURegister::X64MXCSR)
    return RegisterValue{x64::fp::ArchitecturalMXCSRMask, 0};
  return ExecutionBackend::supportedControlBits(R);
}
llvm::Expected<RegisterValue> UnicornBackend::readRegister(CPURegister R) {
  if (!registerMatches(R, architecture()))
    return diagnostic::error(diagnostic::Register);
  RegisterValue V{};
  if (auto E = check(uc_reg_read(State->Engine, registerID(R), V.data()),
                     diagnostic::UnicornReadRegister))
    return std::move(E);
  if (R == CPURegister::X64FPTag) {
    X64FPState FP;
    FP.setFullTag(V[0]);
    V = {FP.Tag, 0};
  }
  return V;
}
llvm::Error UnicornBackend::writeRegister(CPURegister R,
                                          const RegisterValue &V) {
  if (auto E = State->mutableState())
    return E;
  if (!registerMatches(R, architecture()) || !registerValueFits(R, V))
    return diagnostic::error(diagnostic::Register);
  if (R == CPURegister::X64MXCSR && (V[0] & ~x64::fp::ArchitecturalMXCSRMask))
    return diagnostic::error(diagnostic::Register);
  if (isX64FPRegister(R))
    if (auto E = validateX64FPRegister(R, V))
      return E;
  if (R == CPURegister::X64FPTag) {
    // Unicorn takes a full tag word and retains its physical empty bits.
    X64FPState FP;
    FP.Tag = V[0];
    const uint16_t Tag = FP.fullTag();
    return check(uc_reg_write(State->Engine, UC_X86_REG_FPTAG, &Tag),
                 diagnostic::UnicornWriteRegister);
  }
  return check(uc_reg_write(State->Engine, registerID(R), V.data()),
               diagnostic::UnicornWriteRegister);
}

llvm::Expected<std::unique_ptr<BackendContext>> UnicornBackend::saveContext() {
  if (State->FirstFault || State->CallbackFailed || State->MMIOFailed ||
      State->RecoverableFault)
    return failure(unicornDiagnostic::CannotSaveAFaultedCPUInstance);
  auto Saved = std::make_unique<UnicornContext>();
  Saved->Owner = State->Identity;
  Saved->Space = addressSpace();
  if (auto E = check(uc_context_alloc(State->Engine, &Saved->Context),
                     unicornDiagnostic::AllocateCPUContext))
    return std::move(E);
  auto Context = makeContext(std::move(Saved));
  if (auto E = saveContext(*Context))
    return std::move(E);
  return Context;
}

llvm::Error UnicornBackend::saveContext(BackendContext &Context) {
  if (!contextStorage(Context) || contextStorage(Context)->Owner.expired())
    return failure(unicornDiagnostic::CannotSaveToAnExpiredCPUContext);
  if (contextStorage(Context)->Owner.lock() != State->Identity)
    return failure(
        unicornDiagnostic::CPUContextBelongsToAnotherBackendInstance);
  if (State->FirstFault || State->CallbackFailed || State->MMIOFailed ||
      State->RecoverableFault)
    return failure(unicornDiagnostic::CannotSaveAFaultedCPUInstance);
  contextStorage(Context)->Space = addressSpace();
  auto &Saved = static_cast<UnicornContext &>(*contextStorage(Context));
  if (auto E = check(uc_context_save(State->Engine, Saved.Context),
                     unicornDiagnostic::SaveCPUContext))
    return E;
  Saved.Exclusive = State->Exclusive;
  return llvm::Error::success();
}

llvm::Error UnicornBackend::restoreContext(const BackendContext &Context) {
  if (!contextStorage(Context) || contextStorage(Context)->Owner.expired())
    return failure(unicornDiagnostic::CannotRestoreAnExpiredCPUContext);
  if (contextStorage(Context)->Owner.lock() != State->Identity)
    return failure(
        unicornDiagnostic::CPUContextBelongsToAnotherBackendInstance);
  if (State->FirstFault || State->CallbackFailed || State->MMIOFailed ||
      State->RecoverableFault)
    return failure(unicornDiagnostic::CannotRestoreAFaultedCPUInstance);
  if (State->Running)
    return failure(
        unicornDiagnostic::CannotRestoreCPUContextDuringGuestExecution);
  if (auto E = State->Memory->mutableMemory())
    return E;
  if (contextStorage(Context)->Space.lock() != addressSpace())
    return diagnostic::error(diagnostic::ContextSpace);
  if (auto E = check(
          uc_context_restore(State->Engine, static_cast<const UnicornContext &>(
                                                *contextStorage(Context))
                                                .Context),
          unicornDiagnostic::RestoreCPUContext))
    return E;
  State->InstructionPC = State->currentPC();
  State->Exclusive =
      static_cast<const UnicornContext &>(*contextStorage(Context)).Exclusive;
  State->Timeout = false;
  return llvm::Error::success();
}

llvm::Error UnicornBackend::setMemoryWriteWatches(
    const std::vector<MemoryWriteWatch> &Watches) {
  if (auto E = State->mutableState())
    return E;
  if (architecture() != GuestArchitecture::X64)
    return ExecutionBackend::setMemoryWriteWatches(Watches);
  for (const auto &W : Watches)
    if (!W.Size || W.Size - 1 > UINT64_MAX - W.Address)
      return diagnostic::error(diagnostic::WriteWatchRange);
  if (!Watches.empty() && !State->WriteWatchHook) {
    if (auto E = check(uc_hook_add(State->Engine, &State->WriteWatchHook,
                                   UC_HOOK_MEM_WRITE_AFTER,
                                   reinterpret_cast<void *>(Impl::written),
                                   State.get(), 1, 0),
                       unicornDiagnostic::InstallCPUHook))
      return E;
  } else if (Watches.empty() && State->WriteWatchHook) {
    if (auto E = check(uc_hook_del(State->Engine, State->WriteWatchHook),
                       unicornDiagnostic::InstallCPUHook))
      return E;
    State->WriteWatchHook = 0;
  }
  State->Memory->setWriteWatches(Watches);
  State->HasWriteWatches = !Watches.empty();
  return llvm::Error::success();
}

llvm::Expected<uint32_t> UnicornBackend::instructionSize(uint64_t Address) {
  if (auto E = State->mutableState())
    return std::move(E);
  if (architecture() != GuestArchitecture::X64)
    return ExecutionBackend::instructionSize(Address);
  auto Lock = State->Memory->lock();
  if (!Lock)
    return Lock.takeError();
  if (!State->Decoder &&
      cs_open(CS_ARCH_X86, CS_MODE_64, &State->Decoder) != CS_ERR_OK)
    return diagnostic::error(diagnostic::Decode);
  if (!State->Decoded)
    State->Decoded = cs_malloc(State->Decoder);
  if (!State->Decoded)
    return diagnostic::error(diagnostic::Decode);
  std::array<uint8_t, x64::MaxInstructionBytes> Bytes;
  size_t Count = Bytes.size();
  if (Count - 1 > UINT64_MAX - Address)
    Count = UINT64_MAX - Address + 1;
  if (auto Failure = State->Memory->firstAccessFailure(Address, Count, Execute))
    Count = Failure->Address - Address;
  if (!Count)
    return diagnostic::error(diagnostic::Decode);
  if (auto E = State->Memory->read(
          Address, llvm::MutableArrayRef(Bytes).take_front(Count), Execute))
    return std::move(E);
  const uint8_t *Input = Bytes.data();
  if (!cs_disasm_iter(State->Decoder, &Input, &Count, &Address, State->Decoded))
    return diagnostic::error(diagnostic::Decode);
  return State->Decoded->size;
}

llvm::Error UnicornBackend::installHooks(BackendHooks Hooks) {
  if (auto E = State->mutableState())
    return E;
  State->Hooks = std::move(Hooks);
  // Replacing observers must not duplicate the underlying hooks.
  if (!State->HookHandles.empty())
    return llvm::Error::success();
  const std::pair<int, void *> Entries[] = {
      {UC_HOOK_CODE, reinterpret_cast<void *>(Impl::code)},
      {UC_HOOK_MEM_READ, reinterpret_cast<void *>(Impl::read)},
      {UC_HOOK_MEM_WRITE, reinterpret_cast<void *>(Impl::write)},
      {UC_HOOK_MEM_INVALID, reinterpret_cast<void *>(Impl::fault)},
      {UC_HOOK_INTR, reinterpret_cast<void *>(Impl::interrupt)},
      {UC_HOOK_INSN_INVALID,
       reinterpret_cast<void *>(Impl::invalidInstruction)}};
  for (const auto &Entry : Entries) {
    uc_hook Hook = 0;
    if (auto E = check(uc_hook_add(State->Engine, &Hook, Entry.first,
                                   Entry.second, State.get(), 1, 0),
                       unicornDiagnostic::InstallCPUHook))
      return E;
    State->HookHandles.push_back(Hook);
  }
  return llvm::Error::success();
}
llvm::Expected<ExecutionExit> UnicornBackend::runUntilExit(uint64_t PC,
                                                           uint64_t Timeout) {
  bool Started = false;
  auto E = runImpl(PC, Timeout, Started);
  if (!Started) {
    if (E)
      return std::move(E);
    return diagnostic::error(diagnostic::MissingExecutionStart);
  }
  const bool CallbackFailed =
      State->CallbackFailed &&
      (!State->FirstFault ||
       State->FirstFault->Kind == BackendFaultKind::UnhandledException);
  return makeExecutionExit(std::move(E),
                           {.Fault = State->FirstFault,
                            .Recoverable = State->RecoverableFault,
                            .DeviceFailed = State->MMIOFailed,
                            .BackendFailed = CallbackFailed,
                            .InstructionRejected = State->InstructionRejected,
                            .StopRequested = State->StopRequested,
                            .DeadlineReached = State->Timeout});
}

llvm::Error UnicornBackend::runImpl(uint64_t PC, uint64_t TimeoutMicroseconds,
                                    bool &Started) {
  if (auto E = State->deviceError())
    return E;
  if (State->FirstFault || State->CallbackFailed || State->RecoverableFault)
    return failure(unicornDiagnostic::CannotResumeAFaultedCPUInstance);
  if (State->Running)
    return failure(unicornDiagnostic::CannotRecursivelyExecuteACPUInstance);
  // Unicorn treats zero as unbounded and converts microseconds to nanoseconds.
  // Apply the same finite budget admission as checked/native execution before
  // changing projections, registers or observations.
  auto Limit = makeExecutionDeadline(TimeoutMicroseconds);
  if (!Limit)
    return Limit.takeError();
  if (auto E = State->Memory->beginRun(State->Architecture ==
                                               GuestArchitecture::AArch64
                                           ? RAMWriteTracking::Declared
                                           : RAMWriteTracking::Opaque))
    return E;
  auto Release = llvm::scope_exit([&] { State->Memory->endRun(); });
  if (auto E = State->synchronize())
    return E;
  // Host writes through another virtual alias can change executable backing.
  // Flush cached translations at this stopped-CPU boundary before resuming.
  if (auto E = check(uc_ctl_flush_tb(State->Engine),
                     unicornDiagnostic::InvalidateCPUTranslations))
    return E;
  State->InstructionPC = PC;
  State->PendingWriteWatch = false;
  State->Timeout = false;
  State->StopRequested = false;
  State->Running = true;
  Started = true;
  uc_err Status =
      uc_emu_start(State->Engine, PC, UINT64_MAX, TimeoutMicroseconds, 0);
  State->Running = false;
  if (!State->InstructionFailure.empty())
    return failure(State->InstructionFailure);
  if (auto E = State->deviceError())
    return E;
  if (State->RecoverableFault) {
    // Unicorn can report the original exception even after the hook stops the
    // instruction. Only that exact hook-admitted event may be resumed by a
    // caller-supplied exception transfer; all other errors remain terminal.
    const auto &Fault = *State->RecoverableFault;
    const bool Interrupt = Fault.Kind == BackendFaultKind::Interrupt;
    const bool ExpectedStatus =
        Status == UC_ERR_OK ||
        (Interrupt ? Status == UC_ERR_EXCEPTION
         : Fault.Kind == BackendFaultKind::UnmappedMemory
             ? (Fault.Access == BackendAccessKind::Read
                    ? Status == UC_ERR_READ_UNMAPPED
                : Fault.Access == BackendAccessKind::Write
                    ? Status == UC_ERR_WRITE_UNMAPPED
                    : Status == UC_ERR_FETCH_UNMAPPED)
             : Fault.Kind == BackendFaultKind::Protection &&
                   (Fault.Access == BackendAccessKind::Read
                        ? Status == UC_ERR_READ_PROT
                    : Fault.Access == BackendAccessKind::Write
                        ? Status == UC_ERR_WRITE_PROT
                        : Status == UC_ERR_FETCH_PROT));
    if (State->CallbackFailed || State->FirstFault || !ExpectedStatus) {
      State->retain(*State->RecoverableFault);
      State->RecoverableFault.reset();
      return check(Status, unicornDiagnostic::ExecuteGuestAfterException);
    }
    return llvm::Error::success();
  }
  if (Status == UC_ERR_INSN_INVALID || Status == UC_ERR_EXCEPTION)
    State->retain({Status == UC_ERR_INSN_INVALID
                       ? BackendFaultKind::InvalidInstruction
                       : BackendFaultKind::UnhandledException,
                   State->currentPC(), std::nullopt, std::nullopt, std::nullopt,
                   std::nullopt});
  size_t TimedOut = 0;
  if (auto E = check(uc_query(State->Engine, UC_QUERY_TIMEOUT, &TimedOut),
                     unicornDiagnostic::QueryCPUTimeout))
    return E;
  State->Timeout = TimedOut != 0;
  if (State->CallbackFailed)
    return failure(unicornDiagnostic::ExceptionInEmulatorHook);
  return check(Status, unicornDiagnostic::ExecuteGuest);
}
bool UnicornBackend::timedOut() const { return State->Timeout; }
void UnicornBackend::stop() {
  if (State->Running)
    State->StopRequested = true;
  uc_emu_stop(State->Engine);
}
bool UnicornBackend::hasMemoryFault() const {
  return State->FirstFault && State->FirstFault->Access.has_value();
}
bool UnicornBackend::hasDeviceError() const { return State->MMIOFailed; }
std::optional<BackendFault> UnicornBackend::fault() const {
  return State->FirstFault;
}
std::optional<BackendFault> UnicornBackend::takeRecoverableFault() {
  auto Fault = State->RecoverableFault;
  State->RecoverableFault.reset();
  return Fault;
}
bool UnicornBackend::executable(uint64_t Address) const {
  return State->accessible(Address, 1, Execute);
}
} // namespace neverd::emulation
