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

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <exception>
#include <map>
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
  uc_engine *Engine = nullptr;
  uint64_t Limit = 0;
  uint64_t Mapped = 0;
  // The adapter owns permissions for API accesses as uc_mem_read/write bypass
  // guest permissions. CPU accesses use Unicorn's corresponding page metadata.
  std::map<uint64_t, unsigned> Pages;
  BackendHooks Hooks;
  std::vector<uc_hook> HookHandles;
  std::optional<BackendFault> FirstFault;
  uint64_t InstructionPC = 0;
  bool Timeout = false;
  bool CallbackFailed = false;

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
      if (S.Hooks.Write)
        S.Hooks.Write(Address, Size, uint64_t(Value));
    });
  }
  static void read(uc_engine *, uc_mem_type, uint64_t Address, int Size,
                   int64_t, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    S.invoke([&] {
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
    S.retain({Kind, S.currentPC(), Address,
              Size > 0 ? std::optional<uint64_t>(Size) : std::nullopt, Access,
              std::nullopt});
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
  if (auto E = check(uc_mem_map(State->Engine, Address, Size, Permissions),
                     "map guest memory"))
    return E;
  for (uint64_t Offset = 0; Offset < Size; Offset += profile::PageSize)
    State->Pages.emplace(Address + Offset, Permissions);
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
  if (auto E = check(uc_mem_protect(State->Engine, Address, Size, Permissions),
                     "protect guest memory"))
    return E;
  for (uint64_t Offset = 0; Offset < Size; Offset += profile::PageSize)
    State->Pages[Address + Offset] = Permissions;
  return llvm::Error::success();
}

llvm::Error UnicornBackend::read(uint64_t Address,
                                 llvm::MutableArrayRef<uint8_t> Bytes) {
  if (auto Kind = State->accessFault(Address, Bytes.size(), Read)) {
    State->memoryFault(*Kind, BackendAccessKind::Read, Address, Bytes.size());
    return failure("guest read fault at 0x" + llvm::utohexstr(Address));
  }
  return Bytes.empty() ? llvm::Error::success()
                       : check(uc_mem_read(State->Engine, Address, Bytes.data(),
                                           Bytes.size()),
                               "read guest memory");
}
llvm::Error UnicornBackend::write(uint64_t Address,
                                  llvm::ArrayRef<uint8_t> Bytes) {
  if (auto Kind = State->accessFault(Address, Bytes.size(), Write)) {
    State->memoryFault(*Kind, BackendAccessKind::Write, Address, Bytes.size());
    return failure("guest write fault at 0x" + llvm::utohexstr(Address));
  }
  return Bytes.empty() ? llvm::Error::success()
                       : check(uc_mem_write(State->Engine, Address,
                                            Bytes.data(), Bytes.size()),
                               "write guest memory");
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
  if (State->FirstFault || State->CallbackFailed)
    return failure("cannot resume a faulted CPU instance");
  State->InstructionPC = PC;
  State->Timeout = false;
  uc_err Status =
      uc_emu_start(State->Engine, PC, UINT64_MAX, TimeoutMicroseconds, 0);
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
void UnicornBackend::stop() { uc_emu_stop(State->Engine); }
bool UnicornBackend::hasMemoryFault() const {
  return State->FirstFault && State->FirstFault->Access.has_value();
}
std::optional<BackendFault> UnicornBackend::fault() const {
  return State->FirstFault;
}
bool UnicornBackend::executable(uint64_t Address) const {
  return State->accessible(Address, 1, Execute);
}
} // namespace neverd::emulation
