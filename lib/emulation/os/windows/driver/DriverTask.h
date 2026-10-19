//===- DriverTask.h - Suspended driver orchestration ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Suspended driver orchestration.
///

#ifndef NEVERD_EMULATION_DRIVERTASK_H
#define NEVERD_EMULATION_DRIVERTASK_H

#include "neverd/emulation/ExecutionEnvironment.h"

#include <cassert>
#include <coroutine>
#include <exception>
#include <utility>

namespace neverd::emulation {
/// Private orchestration only: tasks never execute a CPU. The one environment
/// resumes its suspended call tree until an action is yielded. Nested tasks
/// preserve the existing model/scheduler stack rather than replaying it.
class DriverTask final {
public:
  struct Control {
    std::coroutine_handle<> Leaf;
    std::optional<EnvironmentAction> Action;
  };
  struct promise_type;
  using Handle = std::coroutine_handle<promise_type>;
  struct Awaiter {
    Handle Child;
    bool await_ready() const noexcept { return Child.done(); }
    std::coroutine_handle<> await_suspend(Handle Parent) noexcept;
    llvm::Error await_resume();
  };
  DriverTask() = default;
  DriverTask(const DriverTask &) = delete;
  DriverTask &operator=(const DriverTask &) = delete;
  DriverTask(DriverTask &&Other) noexcept
      : Coroutine(std::exchange(Other.Coroutine, {})) {}
  DriverTask &operator=(DriverTask &&Other) noexcept;
  ~DriverTask();
  Awaiter operator co_await() && { return {Coroutine}; }
  void bind(Control &);
  bool done() const { return Coroutine && Coroutine.done(); }
  llvm::Error takeResult();

private:
  explicit DriverTask(Handle H) : Coroutine(H) {}
  Handle Coroutine;
};

struct DriverTask::promise_type {
  Control *Owner = nullptr;
  std::coroutine_handle<> Continuation;
  llvm::Error Result = llvm::Error::success();
  std::exception_ptr Exception;
  ~promise_type() { llvm::consumeError(std::move(Result)); }
  DriverTask get_return_object() {
    return DriverTask(Handle::from_promise(*this));
  }
  std::suspend_always initial_suspend() const noexcept { return {}; }
  struct Final {
    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(Handle H) const noexcept {
      auto &P = H.promise();
      if (P.Continuation)
        return P.Continuation;
      if (P.Owner)
        P.Owner->Leaf = {};
      return std::noop_coroutine();
    }
    void await_resume() const noexcept {}
  };
  Final final_suspend() const noexcept { return {}; }
  void return_value(llvm::Error E) { Result = std::move(E); }
  void unhandled_exception() { Exception = std::current_exception(); }
  std::suspend_always yield_value(EnvironmentAction Action) {
    assert(Owner && "driver task has no control owner");
    Owner->Action = std::move(Action);
    Owner->Leaf = Handle::from_promise(*this);
    return {};
  }
};

inline DriverTask &DriverTask::operator=(DriverTask &&Other) noexcept {
  if (this != &Other) {
    if (Coroutine)
      Coroutine.destroy();
    Coroutine = std::exchange(Other.Coroutine, {});
  }
  return *this;
}
inline DriverTask::~DriverTask() {
  if (Coroutine)
    Coroutine.destroy();
}
inline void DriverTask::bind(Control &Owner) {
  assert(Coroutine && !Coroutine.promise().Owner);
  Coroutine.promise().Owner = &Owner;
  Owner.Leaf = Coroutine;
}
inline std::coroutine_handle<>
DriverTask::Awaiter::await_suspend(Handle Parent) noexcept {
  Child.promise().Owner = Parent.promise().Owner;
  Child.promise().Continuation = Parent;
  return Child;
}
inline llvm::Error DriverTask::Awaiter::await_resume() {
  if (Child.promise().Exception)
    std::rethrow_exception(Child.promise().Exception);
  return std::move(Child.promise().Result);
}
inline llvm::Error DriverTask::takeResult() {
  assert(done());
  if (Coroutine.promise().Exception) {
    try {
      std::rethrow_exception(Coroutine.promise().Exception);
    } catch (const std::exception &E) {
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "driver orchestration raised a host exception: %s", E.what());
    } catch (...) {
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "driver orchestration raised a host exception");
    }
  }
  return std::move(Coroutine.promise().Result);
}
} // namespace neverd::emulation
#endif
