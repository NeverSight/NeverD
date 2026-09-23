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

#include "neverd/emulation/DriverProfile.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <exception>
#include <map>
#include <new>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#include <utility>

namespace neverd::emulation {
namespace {
llvm::Error failure(const std::string &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
llvm::Error check(uc_err Error, const char *Operation) {
  if (Error == UC_ERR_OK)
    return llvm::Error::success();
  return failure(std::string(Operation) + ": " + uc_strerror(Error));
}
int registerID(X64Register Register) {
  switch (Register) {
#define NEVERD_UNICORN_REGISTER(Name, Register)                                \
  case X64Register::Name:                                                      \
    return Register;
#include "UnicornRegisters.def"
#undef NEVERD_UNICORN_REGISTER
  }
  return UC_X86_REG_INVALID;
}
} // namespace

const char *backendFaultKindName(BackendFaultKind Kind) {
  switch (Kind) {
#define NEVERD_UNICORN_FAULT_KIND(Name, Spelling)                              \
  case BackendFaultKind::Name:                                                 \
    return Spelling;
#include "UnicornFaults.def"
#undef NEVERD_UNICORN_FAULT_KIND
  }
  llvm_unreachable("unknown backend fault kind");
}

const char *backendAccessKindName(BackendAccessKind Kind) {
  switch (Kind) {
#define NEVERD_UNICORN_ACCESS_KIND(Name, Spelling)                             \
  case BackendAccessKind::Name:                                                \
    return Spelling;
#include "UnicornFaults.def"
#undef NEVERD_UNICORN_ACCESS_KIND
  }
  llvm_unreachable("unknown backend access kind");
}

struct UnicornBackend::Impl {
  struct MMIORegion {
    Impl *Backend;
    uint64_t Address, Size;
    GuestMMIOCallbacks Callbacks;
  };
  uc_engine *Engine = nullptr;
  // A separate identity survives address reuse without extending engine life.
  std::shared_ptr<const void> Identity = std::make_shared<unsigned char>(0);
  uint64_t Limit = 0;
  uint64_t Mapped = 0;
  // The adapter owns permissions for API accesses as uc_mem_read/write bypass
  // guest permissions. CPU accesses use Unicorn's corresponding page metadata.
  std::map<uint64_t, unsigned> Pages;
  std::map<uint64_t, uint8_t *> PageBacking;
  std::vector<std::unique_ptr<uint8_t[]>> OwnedRAM;
  std::map<uint64_t, std::unique_ptr<MMIORegion>> MMIO;
  BackendHooks Hooks;
  std::vector<uc_hook> HookHandles;
  std::optional<BackendFault> FirstFault;
  std::optional<BackendFault> RecoverableFault;
  uint64_t InstructionPC = 0;
  bool Timeout = false;
  bool CallbackFailed = false;
  bool Running = false;
  bool StopRequested = false;
  bool DeviceCallbackActive = false;
  bool MMIOFailed = false;
  std::string MMIOFailure;

  ~Impl() {
    if (Engine)
      uc_close(Engine);
  }

  std::optional<BackendFaultKind> accessFault(uint64_t Address, uint64_t Size,
                                              unsigned Permission) const {
    if (!Size)
      return std::nullopt;
    if (Size - 1 > UINT64_MAX - Address)
      return BackendFaultKind::InvalidMemoryRange;
    uint64_t LastPage = (Address + Size - 1) & ~uint64_t(profile::PageSize - 1);
    for (uint64_t Page = Address & ~uint64_t(profile::PageSize - 1);;) {
      auto I = Pages.find(Page);
      if (I == Pages.end())
        return BackendFaultKind::UnmappedMemory;
      if ((I->second & Permission) != Permission)
        return BackendFaultKind::Protection;
      if (Page == LastPage)
        return std::nullopt;
      Page += profile::PageSize;
    }
  }

  bool accessible(uint64_t Address, uint64_t Size, unsigned Permission) const {
    return !accessFault(Address, Size, Permission);
  }

  MMIORegion *overlappingMMIO(uint64_t Address, uint64_t Size) const {
    if (!Size)
      return nullptr;
    for (const auto &[Base, Region] : MMIO)
      if (Address <= Base ? Base - Address < Size
                          : Address - Base < Region->Size)
        return Region.get();
    return nullptr;
  }

  bool effectsStopped() const {
    return MMIOFailed || CallbackFailed || FirstFault ||
           (Running && StopRequested);
  }

  llvm::Error deviceError() const {
    if (MMIOFailed)
      return failure(MMIOFailure.empty() ? "MMIO callback failed"
                                         : MMIOFailure);
    if (CallbackFailed)
      return failure("exception in emulator hook");
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
      return failure("MMIO access crosses a mapping boundary");
    if ((Size != 1 && Size != 2 && Size != 4) || Address % Size)
      return failure("MMIO requires an aligned 1, 2 or 4 byte transaction");
    if (DeviceCallbackActive)
      return failure("recursive MMIO callback access is unsupported");
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
      S.preflightMMIO(Region.Address + Offset, Size, false);
      if (S.effectsStopped())
        return;
      S.DeviceCallbackActive = true;
      auto Reset = llvm::scope_exit([&] { S.DeviceCallbackActive = false; });
      auto Result = Region.Callbacks.Read(Offset, Size);
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
      S.preflightMMIO(Region.Address + Offset, Size, true);
      if (S.effectsStopped())
        return;
      S.DeviceCallbackActive = true;
      auto Reset = llvm::scope_exit([&] { S.DeviceCallbackActive = false; });
      if (auto E = Region.Callbacks.Write(
              Offset, Size, Value & ((uint64_t(1) << (Size * 8)) - 1)))
        S.failMMIO(std::move(E));
    });
  }

  uint64_t currentPC() const noexcept {
    uint64_t PC = InstructionPC;
    // Capture inside the fault boundary, before a hook can change the CPU.
    if (uc_reg_read(Engine, UC_X86_REG_RIP, &PC) != UC_ERR_OK)
      return InstructionPC;
    return PC;
  }

  void retain(BackendFault Fault) noexcept {
    if (!FirstFault)
      FirstFault = Fault;
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
      if (S.Hooks.Instruction)
        S.Hooks.Instruction(Address, Size);
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
    BackendFault Fault{Kind, S.currentPC(), Address,
                       Size > 0 ? std::optional<uint64_t>(Size) : std::nullopt,
                       Access, std::nullopt};
    S.invoke([&] {
      if (!S.effectsStopped() && S.Hooks.RecoverableFault &&
          S.Hooks.RecoverableFault(Fault)) {
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
    S.retain({BackendFaultKind::Interrupt, S.InstructionPC, std::nullopt,
              std::nullopt, std::nullopt, Number});
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

struct BackendContext::Impl {
  uc_context *Context = nullptr;
  std::weak_ptr<const void> Owner;

  ~Impl() {
    if (Context)
      uc_context_free(Context);
  }
};

BackendContext::BackendContext(std::unique_ptr<Impl> State)
    : State(std::move(State)) {}
BackendContext::~BackendContext() = default;
BackendContext::BackendContext(BackendContext &&) noexcept = default;
BackendContext &BackendContext::operator=(BackendContext &&) noexcept = default;

UnicornBackend::UnicornBackend(std::unique_ptr<Impl> State)
    : State(std::move(State)) {}
UnicornBackend::~UnicornBackend() = default;

llvm::Expected<std::unique_ptr<UnicornBackend>>
UnicornBackend::create(uint64_t MemoryLimit) {
  auto S = std::make_unique<Impl>();
  S->Limit = MemoryLimit;
  if (auto E =
          check(uc_open(UC_ARCH_X86, UC_MODE_64, &S->Engine), "create x64 CPU"))
    return std::move(E);
  // GuestMemory maps virtual addresses directly without Windows page tables.
  // The CPU TLB applies its physical address width even with paging disabled,
  // which truncates canonical kernel addresses. Unicorn's virtual TLB keeps
  // these addresses intact while retaining the mapped page permissions.
  if (auto E = check(uc_ctl_tlb_mode(S->Engine, UC_TLB_VIRTUAL),
                     "configure guest virtual address space"))
    return std::move(E);
  // Scheduling exchanges CPU state while every thread observes the same live
  // address space. Never enable Unicorn's optional memory snapshot mode.
  if (auto E = check(uc_ctl_context_mode(S->Engine, UC_CTL_CONTEXT_CPU),
                     "configure CPU context contents"))
    return std::move(E);
  // No Windows privilege environment is implied by this CPU configuration.
  // DriverSession explicitly rejects environment-dependent instructions.
  uint64_t Flags = profile::InitialRFLAGS;
  if (auto E = check(uc_reg_write(S->Engine, UC_X86_REG_RFLAGS, &Flags),
                     "initialize flags"))
    return std::move(E);
  auto Backend =
      std::unique_ptr<UnicornBackend>(new UnicornBackend(std::move(S)));
  // Core fault capture must also work without optional tracing callbacks.
  if (auto E = Backend->installHooks({}))
    return std::move(E);
  return Backend;
}

llvm::Error UnicornBackend::map(uint64_t Address, uint64_t Size,
                                unsigned Permissions) {
  if (!Size || (Address & (profile::PageSize - 1)) ||
      (Size & (profile::PageSize - 1)) || Size - 1 > UINT64_MAX - Address ||
      (Permissions & ~(Read | Write | Execute)) ||
      Size > State->Limit - State->Mapped)
    return failure("invalid guest mapping or memory limit exceeded");
  for (uint64_t Offset = 0; Offset < Size; Offset += profile::PageSize)
    if (State->Pages.count(Address + Offset))
      return failure("overlapping guest mapping");
  auto Allocation = std::unique_ptr<uint8_t[]>(
      new (std::nothrow) uint8_t[Size + profile::PageSize - 1]());
  if (!Allocation)
    return llvm::make_error<GuestMemoryLimitError>();
  auto *Raw = reinterpret_cast<uint8_t *>(
      (reinterpret_cast<uintptr_t>(Allocation.get()) + profile::PageSize - 1) &
      ~(uintptr_t(profile::PageSize) - 1));
  if (auto E = check(uc_mem_map_ptr(State->Engine, Address, Size, Permissions,
                                    Raw),
                     "map guest memory"))
    return E;
  for (uint64_t Offset = 0; Offset < Size; Offset += profile::PageSize) {
    State->Pages.emplace(Address + Offset, Permissions);
    State->PageBacking.emplace(Address + Offset, Raw + Offset);
  }
  State->OwnedRAM.push_back(std::move(Allocation));
  State->Mapped += Size;
  return llvm::Error::success();
}

llvm::Error UnicornBackend::mapAlias(uint64_t Address, uint64_t Source,
                                     uint64_t Size, unsigned Permissions) {
  if (!Size || (Address & (profile::PageSize - 1)) ||
      (Source & (profile::PageSize - 1)) ||
      (Size & (profile::PageSize - 1)) ||
      Size - 1 > UINT64_MAX - Address || Size - 1 > UINT64_MAX - Source ||
      (Permissions & ~(Read | Write | Execute)) ||
      State->Running || State->effectsStopped())
    return failure("invalid shared RAM alias");
  if (Size > State->Limit - State->Mapped)
    return llvm::make_error<GuestMemoryLimitError>();
  auto First = State->PageBacking.find(Source);
  if (First == State->PageBacking.end())
    return failure("shared RAM alias has no source backing");
  for (uint64_t Offset = 0; Offset < Size; Offset += profile::PageSize) {
    auto Page = State->PageBacking.find(Source + Offset);
    if (State->Pages.count(Address + Offset) ||
        Page == State->PageBacking.end() ||
        Page->second != First->second + Offset)
      return failure("shared RAM alias overlaps or crosses source backing");
  }
  if (auto E = check(uc_mem_map_ptr(State->Engine, Address, Size, Permissions,
                                    First->second),
                     "map shared guest memory"))
    return E;
  for (uint64_t Offset = 0; Offset < Size; Offset += profile::PageSize) {
    State->Pages.emplace(Address + Offset, Permissions);
    State->PageBacking.emplace(Address + Offset, First->second + Offset);
  }
  State->Mapped += Size;
  return llvm::Error::success();
}

llvm::Error UnicornBackend::protect(uint64_t Address, uint64_t Size,
                                    unsigned Permissions) {
  if (!Size || (Address & (profile::PageSize - 1)) ||
      (Size & (profile::PageSize - 1)) ||
      (Permissions & ~(Read | Write | Execute)) ||
      !State->accessible(Address, Size, 0))
    return failure("invalid guest protection range");
  if (State->overlappingMMIO(Address, Size))
    return failure("changing MMIO page permissions is unsupported");
  if (auto E = check(uc_mem_protect(State->Engine, Address, Size, Permissions),
                     "protect guest memory"))
    return E;
  for (uint64_t Offset = 0; Offset < Size; Offset += profile::PageSize)
    State->Pages[Address + Offset] = Permissions;
  return llvm::Error::success();
}

llvm::Error UnicornBackend::mapMMIO(uint64_t Address, uint64_t Size,
                                    GuestMMIOCallbacks Callbacks) {
  if (State->Running || State->DeviceCallbackActive)
    return failure("cannot map MMIO during guest execution or a callback");
  if (!Size || (Address & (profile::PageSize - 1)) ||
      (Size & (profile::PageSize - 1)) || Size - 1 > UINT64_MAX - Address)
    return failure("invalid MMIO mapping");
  if (Size > State->Limit - State->Mapped)
    return llvm::make_error<GuestMemoryLimitError>();
  if (!Callbacks.Validate || !Callbacks.Read || !Callbacks.Write)
    return failure("MMIO mapping requires validate, read and write callbacks");
  for (uint64_t Offset = 0; Offset < Size; Offset += profile::PageSize)
    if (State->Pages.count(Address + Offset))
      return failure("overlapping guest mapping");
  auto Region = std::make_unique<Impl::MMIORegion>(
      Impl::MMIORegion{State.get(), Address, Size, std::move(Callbacks)});
  auto *Identity = Region.get();
  State->MMIO.emplace(Address, std::move(Region));
  if (auto E = check(uc_mmio_map(State->Engine, Address, Size, Impl::mmioRead,
                                 Identity, Impl::mmioWrite, Identity),
                     "map guest MMIO")) {
    State->MMIO.erase(Address);
    return E;
  }
  for (uint64_t Offset = 0; Offset < Size; Offset += profile::PageSize)
    State->Pages.emplace(Address + Offset, Read | Write);
  State->Mapped += Size;
  return llvm::Error::success();
}

llvm::Error UnicornBackend::unmapMMIO(uint64_t Address, uint64_t Size) {
  if (State->Running || State->DeviceCallbackActive)
    return failure("cannot unmap MMIO during guest execution or a callback");
  auto I = State->MMIO.find(Address);
  if (I == State->MMIO.end() || I->second->Size != Size)
    return failure("MMIO unmap requires one exact complete mapping");
  if (auto E =
          check(uc_mem_unmap(State->Engine, Address, Size), "unmap guest MMIO"))
    return E;
  for (uint64_t Offset = 0; Offset < Size; Offset += profile::PageSize)
    State->Pages.erase(Address + Offset);
  State->Mapped -= Size;
  State->MMIO.erase(I);
  return llvm::Error::success();
}

llvm::Error UnicornBackend::read(uint64_t Address,
                                 llvm::MutableArrayRef<uint8_t> Bytes) {
  if (auto E = State->deviceError())
    return E;
  if (auto Kind = State->accessFault(Address, Bytes.size(), Read)) {
    State->memoryFault(*Kind, BackendAccessKind::Read, Address, Bytes.size());
    return failure("guest read fault at 0x" + llvm::utohexstr(Address));
  }
  if (State->overlappingMMIO(Address, Bytes.size()) && State->effectsStopped())
    return failure("cannot access MMIO on a stopped or faulted CPU");
  State->invoke([&] { State->preflightMMIO(Address, Bytes.size(), false); });
  if (auto E = State->deviceError())
    return E;
  auto Status = Bytes.empty() ? UC_ERR_OK
                              : uc_mem_read(State->Engine, Address,
                                            Bytes.data(), Bytes.size());
  if (auto E = State->deviceError())
    return E;
  return check(Status, "read guest memory");
}
llvm::Error UnicornBackend::write(uint64_t Address,
                                  llvm::ArrayRef<uint8_t> Bytes) {
  if (auto E = State->deviceError())
    return E;
  if (auto Kind = State->accessFault(Address, Bytes.size(), Write)) {
    State->memoryFault(*Kind, BackendAccessKind::Write, Address, Bytes.size());
    return failure("guest write fault at 0x" + llvm::utohexstr(Address));
  }
  if (State->overlappingMMIO(Address, Bytes.size()) && State->effectsStopped())
    return failure("cannot access MMIO on a stopped or faulted CPU");
  State->invoke([&] { State->preflightMMIO(Address, Bytes.size(), true); });
  if (auto E = State->deviceError())
    return E;
  auto Status = Bytes.empty() ? UC_ERR_OK
                              : uc_mem_write(State->Engine, Address,
                                             Bytes.data(), Bytes.size());
  if (auto E = State->deviceError())
    return E;
  return check(Status, "write guest memory");
}
llvm::Error UnicornBackend::fetch(uint64_t Address,
                                  llvm::MutableArrayRef<uint8_t> Bytes) {
  if (auto Kind = State->accessFault(Address, Bytes.size(), Execute)) {
    State->memoryFault(*Kind, BackendAccessKind::Execute, Address,
                       Bytes.size());
    return failure("guest fetch fault at 0x" + llvm::utohexstr(Address));
  }
  return check(uc_mem_read(State->Engine, Address, Bytes.data(), Bytes.size()),
               "fetch guest instruction");
}

llvm::Error UnicornBackend::validateBacking(uint64_t Address,
                                            uint64_t Size) const {
  if (State->Running || State->DeviceCallbackActive)
    return failure(
        "RAM backing access requires a stopped CPU without an active "
        "device callback");
  if (State->effectsStopped())
    return failure("cannot access RAM backing on a faulted CPU");
  if (State->accessFault(Address, Size, 0))
    return failure("RAM backing range is unmapped or overflowing");
  if (State->overlappingMMIO(Address, Size))
    return failure("RAM backing access cannot include MMIO");
  return llvm::Error::success();
}

llvm::Expected<bool> UnicornBackend::canAccess(uint64_t Address, uint64_t Size,
                                                unsigned Permissions) const {
  if ((Permissions & ~(Read | Write | Execute)) || State->Running ||
      State->DeviceCallbackActive || State->effectsStopped())
    return failure("CPU access preflight requires a healthy stopped CPU");
  return !State->accessFault(Address, Size, Permissions) &&
         !State->overlappingMMIO(Address, Size);
}

llvm::Error UnicornBackend::readBacking(uint64_t Address,
                                        llvm::MutableArrayRef<uint8_t> Bytes) {
  if (auto E = validateBacking(Address, Bytes.size()))
    return E;
  const uc_err Status = Bytes.empty() ? UC_ERR_OK
                                      : uc_mem_read(State->Engine, Address,
                                                    Bytes.data(), Bytes.size());
  if (Status != UC_ERR_OK)
    State->memoryFault(BackendFaultKind::UnhandledException,
                       BackendAccessKind::Read, Address, Bytes.size());
  return check(Status, "read RAM backing");
}

llvm::Error UnicornBackend::writeBacking(uint64_t Address,
                                         llvm::ArrayRef<uint8_t> Bytes) {
  if (auto E = validateBacking(Address, Bytes.size()))
    return E;
  const uc_err Status =
      Bytes.empty()
          ? UC_ERR_OK
          : uc_mem_write(State->Engine, Address, Bytes.data(), Bytes.size());
  // Unicorn's host write bypasses CPU permissions internally. An unexpected
  // failure does not promise a usable engine or a restored internal readonly
  // state, so preserve the fault and prohibit resume rather than retrying.
  if (Status != UC_ERR_OK)
    State->memoryFault(BackendFaultKind::UnhandledException,
                       BackendAccessKind::Write, Address, Bytes.size());
  return check(Status, "write RAM backing");
}
llvm::Expected<uint64_t> UnicornBackend::reg(X64Register Register) {
  uint64_t Value = 0;
  if (auto E = check(uc_reg_read(State->Engine, registerID(Register), &Value),
                     "read guest register"))
    return std::move(E);
  return Value;
}
llvm::Error UnicornBackend::setReg(X64Register Register, uint64_t Value) {
  return check(uc_reg_write(State->Engine, registerID(Register), &Value),
               "write guest register");
}

llvm::Expected<std::unique_ptr<BackendContext>> UnicornBackend::saveContext() {
  if (State->FirstFault || State->CallbackFailed || State->MMIOFailed)
    return failure("cannot save a faulted CPU instance");
  auto Saved = std::make_unique<BackendContext::Impl>();
  Saved->Owner = State->Identity;
  if (auto E = check(uc_context_alloc(State->Engine, &Saved->Context),
                     "allocate CPU context"))
    return std::move(E);
  auto Context =
      std::unique_ptr<BackendContext>(new BackendContext(std::move(Saved)));
  if (auto E = saveContext(*Context))
    return std::move(E);
  return Context;
}

llvm::Error UnicornBackend::saveContext(BackendContext &Context) {
  if (!Context.State || Context.State->Owner.expired())
    return failure("cannot save to an expired CPU context");
  if (Context.State->Owner.lock() != State->Identity)
    return failure("CPU context belongs to another backend instance");
  if (State->FirstFault || State->CallbackFailed || State->MMIOFailed)
    return failure("cannot save a faulted CPU instance");
  return check(uc_context_save(State->Engine, Context.State->Context),
               "save CPU context");
}

llvm::Error UnicornBackend::restoreContext(const BackendContext &Context) {
  if (!Context.State || Context.State->Owner.expired())
    return failure("cannot restore an expired CPU context");
  if (Context.State->Owner.lock() != State->Identity)
    return failure("CPU context belongs to another backend instance");
  if (State->FirstFault || State->CallbackFailed || State->MMIOFailed)
    return failure("cannot restore a faulted CPU instance");
  if (State->Running)
    return failure("cannot restore CPU context during guest execution");
  if (auto E = check(uc_context_restore(State->Engine, Context.State->Context),
                     "restore CPU context"))
    return E;
  State->InstructionPC = State->currentPC();
  State->Timeout = false;
  return llvm::Error::success();
}

llvm::Error UnicornBackend::installHooks(BackendHooks Hooks) {
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
                       "install CPU hook"))
      return E;
    State->HookHandles.push_back(Hook);
  }
  return llvm::Error::success();
}
llvm::Error UnicornBackend::run(uint64_t PC, uint64_t TimeoutMicroseconds) {
  if (auto E = State->deviceError())
    return E;
  if (State->FirstFault || State->CallbackFailed || State->RecoverableFault)
    return failure("cannot resume a faulted CPU instance");
  if (State->Running)
    return failure("cannot recursively execute a CPU instance");
  State->InstructionPC = PC;
  State->Timeout = false;
  State->StopRequested = false;
  State->Running = true;
  uc_err Status =
      uc_emu_start(State->Engine, PC, UINT64_MAX, TimeoutMicroseconds, 0);
  State->Running = false;
  if (auto E = State->deviceError())
    return E;
  if (State->RecoverableFault) {
    // Unicorn reports the original memory error even after the hook stops the
    // instruction. Only that exact hook-admitted event may be resumed by a
    // caller-supplied exception transfer; all other errors remain terminal.
    if (State->CallbackFailed || State->FirstFault ||
        (Status != UC_ERR_OK && Status != UC_ERR_READ_UNMAPPED &&
         Status != UC_ERR_WRITE_UNMAPPED && Status != UC_ERR_READ_PROT &&
         Status != UC_ERR_WRITE_PROT)) {
      State->retain(*State->RecoverableFault);
      State->RecoverableFault.reset();
      return check(Status, "execute guest after memory exception");
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
                     "query CPU timeout"))
    return E;
  State->Timeout = TimedOut != 0;
  if (State->CallbackFailed)
    return failure("exception in emulator hook");
  return check(Status, "execute guest");
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
