//===- WindowsProcessExceptions.h - User exception continuations ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_PROCESS_EXCEPTIONS_H
#define NEVERD_EMULATION_WINDOWS_PROCESS_EXCEPTIONS_H
#include "../exception/X64SEH.h"
#include "WindowsProcess.h"
#include "WindowsProcessContext.h"
#include "WindowsProcessModules.h"

#include "neverd/emulation/CPU.h"

#include <list>

namespace neverd::emulation::windows_process {
class ExceptionDispatcher final {
public:
  enum class HandlerKind { Exception, Continue };
  using Exception = ServiceOutcome::Exception;
  struct Transfer {
    uint64_t PC;
    std::optional<size_t> CompletedEvent;
  };
  ExceptionDispatcher(ExecutionBackend &CPU, const IntegerABI &ABI,
                      uint64_t StackBase, Program *Modules = nullptr,
                      const ExecutionBudget *Budget = nullptr)
      : CPU(CPU), ABI(ABI), StackBase(StackBase), Modules(Modules),
        Budget(Budget) {}
  llvm::Expected<uint64_t> add(HandlerKind Kind, bool First, uint64_t Handler);
  uint64_t remove(HandlerKind Kind, uint64_t Handle);
  static bool recoverable(GuestArchitecture Architecture,
                          const BackendFault &Fault,
                          std::optional<uint64_t> MXCSR = std::nullopt);
  bool accepts(const BackendFault &Fault) const;
  static std::optional<Exception>
  exception(GuestArchitecture Architecture, const BackendFault &Fault,
            std::optional<uint64_t> MXCSR = std::nullopt);
  llvm::Expected<Transfer> begin(Exception Raised, uint64_t StackPointer,
                                 size_t LoaderDepth,
                                 std::optional<size_t> Event = std::nullopt);
  llvm::Expected<Transfer> beginFault(const BackendFault &Fault,
                                      uint64_t StackPointer,
                                      size_t LoaderDepth);
  bool activeAt(size_t LoaderDepth) const;
  bool returning(uint64_t PC, uint64_t SP, size_t LoaderDepth) const;
  llvm::Expected<Transfer> returned(uint32_t Disposition);
  void abandon() { Frames.clear(); }
  bool hasRuntimeState() const {
    return !Handlers.empty() || !ContinueHandlers.empty() || !Frames.empty();
  }

private:
  llvm::Expected<std::optional<Exception>>
  exception(const BackendFault &) const;
  struct Handler {
    uint64_t Handle, PC;
    bool Live = true;
  };
  struct Unwind {
    struct Image {
      ModuleRef Identity;
      std::shared_ptr<const ExceptionInfo> Metadata;
    };
    std::vector<Image> Images;
    std::unique_ptr<X64SEH> Planner;
    X64SEH::Dispatch Cursor;
    std::optional<X64SEH::Action> Callback;
    std::vector<size_t> Origins;
    std::vector<uint8_t> Record;
    uint32_t Flags = 0;
  };
  struct Frame {
    std::unique_ptr<BackendContext> Snapshot;
    std::vector<uint8_t> Context;
    std::list<Handler>::iterator Current;
    uint64_t Top, Payload, ExpectedSP;
    size_t LoaderDepth;
    std::optional<size_t> Event;
    HandlerKind Kind = HandlerKind::Exception;
    std::unique_ptr<Unwind> SEH;
    std::optional<size_t> Rejected;
    ContextOrigin Origin = ContextOrigin::Current;
  };
  llvm::Expected<Transfer>
  beginDispatch(Exception Raised, uint64_t StackPointer, size_t LoaderDepth,
                std::optional<size_t> Event, std::optional<size_t> Rejected,
                ContextOrigin Origin = ContextOrigin::Current);
  std::list<Handler> &handlers(HandlerKind Kind);
  llvm::Expected<Transfer> callNext();
  llvm::Expected<Transfer> continueExecution();
  llvm::Expected<Transfer> startUnwind();
  bool hasFrameHandlers() const;
  llvm::Expected<Transfer> advanceUnwind(std::optional<int32_t> Filter = {});
  llvm::Error validateUnwind();
  llvm::Error writeUnwindRecord();
  llvm::Expected<Transfer> raiseNoncontinuable();
  void collect();
  ExecutionBackend &CPU;
  IntegerABI ABI;
  uint64_t StackBase, NextHandle = value::ExceptionHandleBase;
  std::list<Handler> Handlers, ContinueHandlers;
  std::vector<Frame> Frames;
  Program *Modules;
  const ExecutionBudget *Budget;
};
} // namespace neverd::emulation::windows_process
#endif
