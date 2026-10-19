//===- NeverDCAPIExecution.cpp - Persistent C workload boundary
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Persistent C workload boundary.
///

#include "neverd/sdk/NeverDCAPIExecution.h"

#include "ResourceHandles.h"

#include "neverd/emulation/ExecutionCommand.h"
#include "neverd/emulation/ExecutionRunJSON.h"
#ifdef NEVERD_ENABLE_DRIVER_EMULATION
#include "neverd/emulation/DriverExecutionFactory.h"
#endif

#ifdef NEVERD_ENABLE_CPU_EMULATION
#include "ExecutionHandle.h"
#endif
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <cstring>

namespace r = neverd::sdk::resources;
namespace e = neverd::emulation;
namespace {
llvm::Error failure(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
#ifdef NEVERD_ENABLE_CPU_EMULATION
llvm::Expected<std::shared_ptr<r::ExecutionHandleState>> state(uint64_t ID) {
  auto S = r::acquire(ID, r::HandleKind::Execution);
  if (!S)
    return S.takeError();
  return std::static_pointer_cast<r::ExecutionHandleState>(*S);
}
#endif
template <typename T, typename F>
llvm::Expected<T> access(uint64_t ID, bool Stopped, F &&Body) {
#ifdef NEVERD_ENABLE_CPU_EMULATION
  auto S = state(ID);
  if (!S)
    return S.takeError();
  if (!Stopped)
    return Body(**S);
  if ((*S)->Busy.exchange(true))
    return failure("execution is busy in another operation");
  auto Release = llvm::scope_exit([&] { (*S)->Busy.store(false); });
  auto Sources = r::accessModule((*S)->Sources);
  if (!Sources)
    return Sources.takeError();
  return Body(**S);
#else
  return failure("persistent CPU execution is disabled in this build");
#endif
}
template <typename F>
int mutate(neverd_execution_t ID, const char **Error, F &&Body) {
  return r::boundary<int>(Error, 0, [&]() -> llvm::Expected<int> {
    return access<int>(ID, true, [&](auto &S) -> llvm::Expected<int> {
      if (auto E = Body(S))
        return std::move(E);
      return 1;
    });
  });
}
llvm::Expected<const char *> report(llvm::Expected<std::string> Text) {
  if (!Text)
    return Text.takeError();
  return r::ownedString(*Text, e::execution_limit::JSONOutputBytes);
}
template <typename F>
const char *inspect(neverd_execution_t ID, const char **Error, F &&Body) {
  return r::boundary<const char *>(
      Error, nullptr, [&]() -> llvm::Expected<const char *> {
        return access<const char *>(ID, true,
                                    [&](auto &S) { return report(Body(S)); });
      });
}
} // namespace

extern "C" const char *neverd_execution_command_json(neverd_execution_t ID,
                                                     const char *JSON,
                                                     const char **Error) {
  return r::boundary<
      const char *>(Error, nullptr, [&]() -> llvm::Expected<const char *> {
#ifdef NEVERD_ENABLE_CPU_EMULATION
    // Pin immutable creation metadata through parsing/dispatch. The selected
    // primitive acquires its own guard and can reject a concurrent destroy.
    auto Owner = state(ID);
    if (!Owner)
      return Owner.takeError();
    auto Text =
        r::text(JSON, NEVERD_EXECUTION_JSON_LIMIT, "execution command JSON");
    if (!Text)
      return Text.takeError();
    auto C = e::executionCommandFromJSON(
        *Text, (*Owner)->Workload.MemoryTransferLimit);
    if (!C)
      return C.takeError();
    auto Result = [&]() -> llvm::Expected<std::string> {
      auto Report = [&](auto &&Call) -> llvm::Expected<std::string> {
        const char *Failure = nullptr;
        const char *Data = Call(&Failure);
        auto Cleanup = llvm::scope_exit([&] {
          neverd_free_string(Data);
          neverd_free_string(Failure);
        });
        if (!Data || Failure)
          return failure(Failure ? Failure : "execution report failed");
        return std::string(Data);
      };
      auto Action = [&](auto &&Call) -> llvm::Expected<std::string> {
        const char *Failure = nullptr;
        const int OK = Call(&Failure);
        auto Cleanup = llvm::scope_exit([&] { neverd_free_string(Failure); });
        if (!OK || Failure)
          return failure(Failure ? Failure : "execution action failed");
        return std::string("{\"done\":true}");
      };
      using K = e::ExecutionCommandKind;
      switch (C->Kind) {
      case K::Run:
        return Report([&](auto E) { return neverd_execution_run_json(ID, E); });
      case K::State:
        return Report(
            [&](auto E) { return neverd_execution_state_json(ID, E); });
      case K::Pause:
        return Action(
            [&](auto E) { return neverd_execution_request_pause(ID, E); });
      case K::Resume:
        return Action([&](auto E) { return neverd_execution_resume(ID, E); });
      case K::Inspect:
        return Report(
            [&](auto E) { return neverd_execution_inspect_json(ID, E); });
      case K::Instances:
        return Report(
            [&](auto E) { return neverd_execution_instances_json(ID, E); });
      case K::Declare:
        return Action([&](auto E) {
          return neverd_execution_declare_binding_json(ID, C->Payload.c_str(),
                                                       E);
        });
      case K::Supply:
        return Action([&](auto E) {
          return neverd_execution_supply_json(ID, C->Payload.c_str(), E);
        });
      case K::Invoke:
        return Action([&](auto E) {
          return neverd_execution_invoke_json(ID, C->Payload.c_str(), E);
        });
      case K::Lookup:
        return Report([&](auto E) {
          return neverd_execution_lookup_symbol_json(ID, C->InstanceID.c_str(),
                                                     C->Symbol.c_str(), E);
        });
      case K::Read: {
        std::vector<uint8_t> Bytes(C->Size);
        auto OK = Action([&](auto E) {
          return neverd_execution_read_memory(ID, C->Address, Bytes.data(),
                                              Bytes.size(), E);
        });
        if (!OK)
          return OK.takeError();
        return llvm::formatv(
                   "{0}", llvm::json::Value(llvm::json::Object{
                              {"address", "0x" + llvm::utohexstr(C->Address)},
                              {"bytes_hex", llvm::toHex(Bytes, true)}}))
            .str();
      }
      case K::Write:
        return Action([&](auto E) {
          return neverd_execution_write_memory(ID, C->Address, C->Bytes.data(),
                                               C->Bytes.size(), E);
        });
      case K::Modules:
      case K::ModuleAdd:
      case K::ModuleRevise:
      case K::ModuleDebug:
        return access<std::string>(
            ID, true, [&](auto &S) -> llvm::Expected<std::string> {
              llvm::Error E = llvm::Error::success();
              if (C->Kind == K::ModuleAdd)
                E = r::addModuleJSON(*S.Sources, C->Payload.c_str());
              else if (C->Kind == K::ModuleRevise)
                E = r::reviseModuleJSON(*S.Sources, C->Payload.c_str());
              else if (C->Kind == K::ModuleDebug)
                E = r::attachModuleDebugJSON(*S.Sources, C->ModuleID.c_str(),
                                             C->Payload.c_str());
              if (E)
                return std::move(E);
              if (C->Kind == K::Modules)
                return neverd::modules::moduleSetJSON(*S.Sources->Sources);
              return std::string("{\"done\":true}");
            });
      case K::Quit:
        return std::string("{\"quit\":true}");
      }
      return failure("unsupported execution command");
    }();
    if (!Result)
      return Result.takeError();
    auto Value = llvm::json::parse(*Result);
    if (!Value)
      return Value.takeError();
    return r::ownedString(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"schema_version", 1},
                                 {"id", C->ID},
                                 {"ok", true},
                                 {"result", std::move(*Value)}}))
            .str(),
        NEVERD_EXECUTION_REPORT_LIMIT);
#else
    return failure("persistent CPU execution is disabled in this build");
#endif
  });
}
extern "C" neverd_execution_t
neverd_execution_create_v1(const neverd_execution_options_v1 *Options,
                           const char **Error) {
  return r::boundary<neverd_execution_t>(
      Error, 0, [&]() -> llvm::Expected<neverd_execution_t> {
        if (!Options || Options->struct_size != sizeof(*Options))
          return failure("execution options require exact v1 struct_size");
        auto Text =
            r::text(Options->configuration_json, e::execution_limit::JSONBytes,
                    "execution configuration JSON");
        if (!Text)
          return Text.takeError();
#ifdef NEVERD_ENABLE_CPU_EMULATION
        auto Input = e::executionCreationFromJSON(*Text);
        if (!Input)
          return Input.takeError();
        auto Sources = r::accessModule(Options->module_set);
        if (!Sources)
          return Sources.takeError();
        auto Workload = [&]() -> llvm::Expected<e::ExecutionRunFactoryResult> {
          if (Input->EnvironmentKind == "buffer")
            return e::createBufferExecution(std::move(*Input),
                                            Sources->state().Sources);
          if (Input->EnvironmentKind == "windows-driver") {
#ifdef NEVERD_ENABLE_DRIVER_EMULATION
            return e::createDriverExecution(std::move(*Input),
                                            Sources->state().Sources,
                                            Sources->state().BaseDirectory);
#else
            return failure(
                "Windows driver emulation is disabled in this build");
#endif
          }
          return failure("unsupported execution environment: " +
                         Input->EnvironmentKind);
        }();
        if (!Workload)
          return Workload.takeError();
        auto S = std::make_shared<r::ExecutionHandleState>();
        S->Sources = Sources->sharedState();
        S->Workload = std::move(*Workload);
        return r::retain(r::HandleKind::Execution, std::move(S));
#else
    return failure("persistent CPU execution is disabled in this build");
#endif
      });
}
extern "C" int neverd_execution_destroy(neverd_execution_t ID,
                                        const char **Error) {
  return r::boundary<int>(Error, 0, [&]() -> llvm::Expected<int> {
    if (!ID)
      return 1;
    return access<int>(ID, true, [&](auto &) -> llvm::Expected<int> {
      if (auto E = r::release(ID, r::HandleKind::Execution))
        return std::move(E);
      return 1;
    });
  });
}
extern "C" const char *neverd_execution_state_json(neverd_execution_t ID,
                                                   const char **Error) {
  return r::boundary<const char *>(
      Error, nullptr, [&]() -> llvm::Expected<const char *> {
        return access<const char *>(
            ID, false, [&](auto &S) -> llvm::Expected<const char *> {
              return r::ownedString(
                  llvm::formatv("{0}",
                                llvm::json::Value(llvm::json::Object{
                                    {"schema_version", 1},
                                    {"state", e::executionRunStateName(
                                                  S.Workload.Run->state())}}))
                      .str());
            });
      });
}
extern "C" int neverd_execution_request_pause(neverd_execution_t ID,
                                              const char **Error) {
  return r::boundary<int>(Error, 0, [&]() -> llvm::Expected<int> {
    return access<int>(ID, false, [&](auto &S) -> llvm::Expected<int> {
      if (auto E = S.Workload.Run->requestPause())
        return std::move(E);
      return 1;
    });
  });
}
extern "C" const char *neverd_execution_run_json(neverd_execution_t ID,
                                                 const char **Error) {
  return inspect(ID, Error, [](auto &S) -> llvm::Expected<std::string> {
    auto Event = S.Workload.Run->run();
    if (!Event)
      return Event.takeError();
    return e::executionRunEventJSON(*Event, S.Workload.IdentifySpace);
  });
}
extern "C" int neverd_execution_resume(neverd_execution_t ID,
                                       const char **Error) {
  return mutate(ID, Error, [](auto &S) { return S.Workload.Run->resume(); });
}
extern "C" const char *neverd_execution_inspect_json(neverd_execution_t ID,
                                                     const char **Error) {
  return inspect(ID, Error, [](auto &S) -> llvm::Expected<std::string> {
    auto Snapshot = S.Workload.Run->inspect();
    if (!Snapshot)
      return Snapshot.takeError();
    auto Text =
        e::executionRunSnapshotJSON(*Snapshot, S.Workload.IdentifySpace);
    if (!Text || !S.Workload.InspectEnvironment)
      return Text;
    auto Environment = S.Workload.InspectEnvironment();
    if (!Environment)
      return Environment.takeError();
    auto Report = llvm::json::parse(*Text);
    if (!Report)
      return Report.takeError();
    auto Value = llvm::json::parse(*Environment);
    if (!Value)
      return Value.takeError();
    (*Report->getAsObject())["environment_report"] = std::move(*Value);
    return llvm::formatv("{0}", *Report).str();
  });
}
extern "C" const char *neverd_execution_instances_json(neverd_execution_t ID,
                                                       const char **Error) {
  return inspect(ID, Error, [](auto &S) {
    return e::executionInstancesJSON(S.Workload.Run->modules().inspect(),
                                     S.Workload.IdentifySpace);
  });
}
extern "C" int neverd_execution_declare_binding_json(neverd_execution_t ID,
                                                     const char *JSON,
                                                     const char **Error) {
  return mutate(ID, Error, [&](auto &S) -> llvm::Error {
    auto Text = r::text(JSON, e::execution_limit::JSONBytes, "binding JSON");
    if (!Text)
      return Text.takeError();
    auto Inputs = e::executionBindingsFromJSON("[" + Text->str() + "]");
    if (!Inputs)
      return Inputs.takeError();
    if (Inputs->size() != 1)
      return failure("declare binding requires one object");
    auto Bindings =
        e::resolveExecutionBindings(*Inputs, S.Workload.ResolveSpace);
    if (!Bindings)
      return Bindings.takeError();
    return S.Workload.Run->declareModule(std::move(Bindings->front()));
  });
}
extern "C" int neverd_execution_supply_json(neverd_execution_t ID,
                                            const char *JSON,
                                            const char **Error) {
  return mutate(ID, Error, [&](auto &S) -> llvm::Error {
    auto Text =
        r::text(JSON, e::execution_limit::JSONBytes, "resource supply JSON");
    if (!Text)
      return Text.takeError();
    auto Input = e::executionSupplyFromJSON(*Text);
    if (!Input)
      return Input.takeError();
    auto Bindings =
        e::resolveExecutionBindings(Input->Bindings, S.Workload.ResolveSpace);
    if (!Bindings)
      return Bindings.takeError();
    return S.Workload.Run->supply(Input->Token, *Bindings);
  });
}
extern "C" int neverd_execution_invoke_json(neverd_execution_t ID,
                                            const char *JSON,
                                            const char **Error) {
  return mutate(ID, Error, [&](auto &S) -> llvm::Error {
    auto Text = r::text(JSON, e::execution_limit::JSONBytes, "invocation JSON");
    if (!Text)
      return Text.takeError();
    auto Input =
        e::guestInvocationFromJSON(*Text, S.Workload.MemoryTransferLimit);
    if (!Input)
      return Input.takeError();
    return S.Workload.Run->invoke(std::move(*Input));
  });
}
extern "C" int neverd_execution_read_memory(neverd_execution_t ID,
                                            uint64_t Address, uint8_t *Bytes,
                                            size_t Size, const char **Error) {
  return mutate(ID, Error, [&](auto &S) -> llvm::Error {
    if ((!Bytes && Size) || Size > S.Workload.MemoryTransferLimit)
      return failure(
          "memory read requires a caller buffer within the transfer limit");
    auto Data = S.Workload.Run->readMemory(Address, Size);
    if (!Data)
      return Data.takeError();
    if (Size)
      std::memcpy(Bytes, Data->data(), Size);
    return llvm::Error::success();
  });
}
extern "C" int neverd_execution_write_memory(neverd_execution_t ID,
                                             uint64_t Address,
                                             const uint8_t *Bytes, size_t Size,
                                             const char **Error) {
  return mutate(ID, Error, [&](auto &S) -> llvm::Error {
    if ((!Bytes && Size) || Size > S.Workload.MemoryTransferLimit)
      return failure(
          "memory write requires a caller buffer within the transfer limit");
    return S.Workload.Run->writeMemory(Address,
                                       llvm::ArrayRef<uint8_t>(Bytes, Size));
  });
}
extern "C" const char *
neverd_execution_lookup_symbol_json(neverd_execution_t ID,
                                    const char *InstanceID, const char *Name,
                                    const char **Error) {
  return inspect(ID, Error, [&](auto &S) -> llvm::Expected<std::string> {
    auto Instance =
        r::text(InstanceID, e::execution_limit::NameBytes, "instance id");
    if (!Instance)
      return Instance.takeError();
    auto Symbol = r::text(Name, NEVERD_MODULE_SYMBOL_LIMIT, "symbol name");
    if (!Symbol)
      return Symbol.takeError();
    auto Address = S.Workload.Run->modules().lookupSymbol(*Instance, *Symbol);
    if (!Address)
      return Address.takeError();
    llvm::json::Object O{{"schema_version", 1},
                         {"instance_id", Instance->str()},
                         {"symbol", Symbol->str()},
                         {"found", Address->has_value()}};
    if (*Address)
      O["address"] = "0x" + llvm::utohexstr(**Address);
    return llvm::formatv("{0}", llvm::json::Value(std::move(O))).str();
  });
}
