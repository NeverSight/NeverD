//===- AndroidThreadCalls.cpp - Bionic thread call boundary --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidThreads.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
BionicResult GuestThreads::create(const NativeCallEvent &Call) {
  using namespace thread_attribute_abi;
  const auto &A = Call.Arguments;
  Thread Child;
  Child.Attributes = defaultAttributes();
  if (A[1]) {
    if (auto E = access(A[1], ObjectBytes, Read))
      return std::move(E);
    if (auto E = CPU.read(A[1], Child.Attributes))
      return std::move(E);
  }
  const auto *Bytes = Child.Attributes.data();
  const uint32_t Flags = llvm::support::endian::read32le(Bytes + FlagsOffset);
  const uint64_t Base =
      llvm::support::endian::read64le(Bytes + StackBaseOffset);
  uint64_t Size = llvm::support::endian::read64le(Bytes + StackSizeOffset);
  uint64_t Guard = llvm::support::endian::read64le(Bytes + GuardSizeOffset);
  const uint32_t Policy = llvm::support::endian::read32le(Bytes + PolicyOffset);
  const uint32_t Priority =
      llvm::support::endian::read32le(Bytes + PriorityOffset);
  // Explicit model subset. Inherited policy is always the parent's normal
  // policy. Legacy API 28 flags also ignore priority for SCHED_NORMAL.
  const bool SetPolicy = !(Flags & InheritFlag) &&
                         ((Flags & ExplicitFlag) || Policy != NormalPolicy);
  if ((Flags & ~(DetachedFlag | InheritFlag | ExplicitFlag)) || Base ||
      (SetPolicy && (Policy != NormalPolicy || Priority)))
    return unsupported(diagnostic::ThreadAttributes);
  if (Threads.size() >= Options.Android->ThreadLimit ||
      Size < MinimumStackSize ||
      Size > thread_model::ArenaStride - 3 * PageSize ||
      Guard > thread_model::ArenaStride - Size - 3 * PageSize)
    return value(linux_model::TryAgain);
  Size = (Size + PageSize - 1) & ~(PageSize - 1);
  Guard = (Guard + PageSize - 1) & ~(PageSize - 1);
  auto &Space = *CPU.addressSpace();
  const uint64_t MappingSize = Guard + Size + PageSize;
  if (MappingSize > Options.MemoryLimit - Space.mappedBytes())
    return value(linux_model::TryAgain);
  if (auto E = access(A[0], 8, Write))
    return std::move(E);
  if (A[2] % 4)
    return failure(diagnostic::ThreadEntry);
  if (auto E = access(A[2], 4, Execute, 4))
    return std::move(E);

  // The first page in each slot is an unmapped opaque handle, not a guessed
  // pthread_internal_t. Stack guards have mapped PROT_NONE storage; TLS follows
  // the stack in this documented model placement.
  const uint64_t Handle = thread_model::ArenaBase +
                          (Threads.size() - 1) * thread_model::ArenaStride;
  const uint64_t MappingBase = Handle + PageSize;
  if (auto E =
          Space.map(MappingBase, MappingSize, Read | Write | UserAccessible)) {
    if (!E.isA<GuestMemoryLimitError>())
      return std::move(E);
    llvm::consumeError(std::move(E));
    return value(linux_model::TryAgain);
  }
  auto Rollback = llvm::scope_exit(
      [&] { llvm::consumeError(Space.unmap(MappingBase, MappingSize)); });
  if (Guard)
    if (auto E = Space.protect(MappingBase, Guard, 0))
      return std::move(E);
  Child.MappingBase = MappingBase;
  Child.MappingSize = MappingSize;
  Child.Report = {linux_model::ThreadID + Threads.size(),
                  Handle,
                  MappingBase + Guard + Size,
                  MappingBase,
                  Guard + Size,
                  Guard};
  Child.Report.Detached = Flags & DetachedFlag;
  Child.Kernel.ID = Child.Report.ID;
  stackAttributes(Child.Attributes, Child.Report);
  if (auto E = put64(Child.Report.TLS, Child.Report.TLS))
    return std::move(E);
  if (auto E = put64(Child.Report.TLS + thread_model::IdentityOffset, Handle))
    return std::move(E);
  if (auto E = put64(Child.Report.TLS + GuardAddress - TLSAddress, StackGuard))
    return std::move(E);

  // Creating the entry state executes no child instructions. Save and restore
  // the parent in full, including its live stack, flags, vectors and FP mode.
  auto Parent = CPU.saveContext();
  if (!Parent)
    return Parent.takeError();
  auto Prepare = [&]() -> llvm::Error {
    auto Frame =
        Calls.prepareCall(CPU, MappingBase + Guard, Size, ReturnPC, {A[3]});
    if (!Frame)
      return Frame.takeError();
    if (auto E = CPU.writeRegister(CPURegister::AArch64TPIDR_EL0,
                                   {Child.Report.TLS, 0}))
      return E;
    if (auto E = CPU.writeRegister(CPURegister::AArch64PC, {A[2], 0}))
      return E;
    auto Context = CPU.saveContext();
    if (!Context)
      return Context.takeError();
    Child.Context = std::move(*Context);
    return llvm::Error::success();
  };
  auto Prepared = Prepare();
  if (auto E =
          llvm::joinErrors(std::move(Prepared), CPU.restoreContext(**Parent)))
    return std::move(E);
  // Publish only after all input, memory and CPU preparation succeeds.
  const uint64_t ChildID = Child.Kernel.ID;
  if (auto E = PriorityState.inherit(id(), ChildID))
    return std::move(E);
  auto CancelPriority =
      llvm::scope_exit([&] { PriorityState.cancel(ChildID); });
  if (auto E = put64(A[0], Handle))
    return std::move(E);
  Threads.push_back(std::move(Child));
  Signals.inherit(id(), ChildID);
  CancelPriority.release();
  Rollback.release();
  return value(0);
}

llvm::Expected<size_t> GuestThreads::findHandle(uint64_t Handle) const {
  for (size_t Index = 0; Index < Threads.size(); ++Index)
    if (Threads[Index].Report.Handle == Handle &&
        !Threads[Index].Report.Retired)
      return Index;
  // API 28's target>=O invalid non-null pthread_t boundary is fatal.
  return failure(diagnostic::ThreadHandle);
}

BionicResult GuestThreads::self(const NativeCallEvent &) {
  return value(current().Report.Handle);
}

BionicResult GuestThreads::equal(const NativeCallEvent &Call) {
  return value(Call.Arguments[0] == Call.Arguments[1]);
}

BionicResult GuestThreads::exit(const NativeCallEvent &Call) {
  if (!callbacks().empty())
    return unsupported(diagnostic::ThreadCallbackExit);
  return std::optional<BionicValue>(GuestThreadExit{Call.Arguments[0]});
}

BionicResult GuestThreads::getTID(const NativeCallEvent &Call) {
  if (!Call.Arguments[0])
    return value(UINT32_MAX);
  auto Index = findHandle(Call.Arguments[0]);
  if (!Index)
    return Index.takeError();
  const auto &Target = Threads[*Index];
  return value(Target.Report.Finished ? 0 : Target.Kernel.ID);
}

BionicResult GuestThreads::getAttributes(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (!A[0])
    return value(linux_model::NoSuchProcess);
  auto Index = findHandle(A[0]);
  if (!Index)
    return Index.takeError();
  const auto &Target = Threads[*Index];
  if (auto E = access(A[1], Target.Attributes.size(), Write))
    return std::move(E);
  auto Bytes = Target.Attributes;
  stackAttributes(Bytes, Target.Report);
  if (auto E = CPU.write(A[1], Bytes))
    return std::move(E);
  return value(0);
}

BionicResult GuestThreads::detach(const NativeCallEvent &Call) {
  if (!Call.Arguments[0])
    return value(linux_model::NoSuchProcess);
  auto Index = findHandle(Call.Arguments[0]);
  if (!Index)
    return Index.takeError();
  auto &Target = Threads[*Index];
  if (Target.Report.Detached || Target.Joiner)
    return value(linux_model::InvalidArgument);
  Target.Report.Detached = true;
  if (Target.Report.Finished)
    if (auto E = retire(*Index))
      return std::move(E);
  return value(0);
}

BionicResult GuestThreads::join(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (A[0] == current().Report.Handle)
    return value(linux_model::Deadlock);
  if (!A[0])
    return value(linux_model::NoSuchProcess);
  auto Index = findHandle(A[0]);
  if (!Index)
    return Index.takeError();
  auto &Target = Threads[*Index];
  if (Target.Report.Detached || Target.Joiner)
    return value(linux_model::InvalidArgument);
  if (A[1])
    if (auto E = access(A[1], 8, Write))
      return std::move(E);
  Target.Joiner = Current;
  current().Waiting = Wait{Join{*Index, A[1]}, std::nullopt};
  if (Target.Report.Finished) {
    if (auto E = prepareJoin(std::get<Join>(current().Waiting->Operation)))
      return std::move(E);
    if (auto E = completeWait(0))
      return std::move(E);
    return value(0);
  }
  return std::optional<BionicValue>(GuestThreadWait{});
}

BionicResult GuestThreads::invoke(const NativeCallEvent &Call) {
  if (!enabled())
    return unsupported(diagnostic::ThreadDisabled);
  using Handler = BionicResult (GuestThreads::*)(const NativeCallEvent &);
  static const llvm::StringMap<Handler> Handlers{
#define NEVERD_ANDROID_THREAD_CALL(Name, Text, Method)                         \
  {Text, &GuestThreads::Method},
#include "AndroidThreads.def"
#undef NEVERD_ANDROID_THREAD_CALL
  };
  auto I = Handlers.find(Call.Name);
  if (I == Handlers.end())
    return unsupported(diagnostic::ThreadOperation);
  return (this->*I->second)(Call);
}

BionicResult Bionic::threadCall(const NativeCallEvent &Call) {
  if (!Threads || !Threads->enabled())
    return unsupportedImport(Call);
  return Threads->invoke(Call);
}

} // namespace neverd::emulation::android_model
