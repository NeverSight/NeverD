//===- AndroidBinderDispatch.cpp - Incoming Java Binder -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 JavaBBinder upcalls reuse Runtime activations and Parcel ownership.
//===----------------------------------------------------------------------===//

#include "AndroidFramework.h"

#include "neverd/dalvik/Limits.h"

#include <memory>
#include <string>
#include <utility>

namespace neverd::mobile::dalvik {
ExecutionResult
AndroidFramework::dispatchBinder(Heap &heap, const Program &program,
                                 BinderIncoming incoming,
                                 const AdmitInstruction &admit) {
  auto unsupported = [](std::string text) {
    return ExecutionResult{ExecutionStop::Unsupported, 0, {}, std::move(text)};
  };
  if (!incoming.pointer)
    return unsupported(
        "Android incoming context Binder association is not supplied");
  auto found = local_binders.find(incoming.cookie);
  if (found == local_binders.end() || found->second.pointer != incoming.pointer)
    return unsupported("Android incoming Binder has no local Java association");
  auto valid = services.binder_identity(BinderIdentityOperation::Validate,
                                        incoming.pointer);
  if (valid.stop != ExecutionStop::Returned)
    return valid;
  if (incoming.flags & ~uint32_t(0x10))
    return unsupported(
        "Android incoming Binder flags require an unmodeled transport");
  if (incoming.data.status || !incoming.data.bytes)
    return {ExecutionStop::Invalid,
            0,
            {},
            "Android incoming Binder data is missing"};

  auto data = std::make_shared<ParcelState>();
  auto reply = std::make_shared<ParcelState>();
  // Unregistered byte owners share Heap's budget without Java identities.
  data->storage = std::make_unique<Object>();
  reply->storage = std::make_unique<Object>();
  auto replaced =
      replaceParcel(heap, *data, *data->storage, std::move(incoming.data));
  if (replaced.stop != ExecutionStop::Returned)
    return replaced;
  // Native Parcels outlive their Java wrappers and the BC_REPLY write. Their
  // destructors run only when sendReply's waitForResponse has acknowledged it.
  auto respond = [this, &heap, data, reply](int32_t status) {
    if (!status && !reply->objects.empty() && reply->fd &&
        reply->fd != data->fd)
      return ExecutionResult{ExecutionStop::Unsupported,
                             0,
                             {},
                             "Android Binder reply objects belong to a "
                             "different native connection"};
    if (!status && reply->position > reply->storage->Bytes.size())
      return ExecutionResult{
          ExecutionStop::Unsupported,
          0,
          {},
          "Android Binder reply requires uninitialized Parcel bytes"};
    reply->position = 0; // BBinder::transact, after JavaBBinder::onTransact.
    ExecutionResult result{ExecutionStop::Suspended};
    result.external.emplace();
    result.external->kind = ExternalCall::BinderSendReply;
    result.external->response = BinderResponse{
        status ? status : reply->error, reply->storage->Bytes, reply->objects};
    result.external->complete = [this, &heap, data,
                                 reply](ExecutionResult completed) {
      if (completed.stop != ExecutionStop::Returned)
        return completed;
      for (const auto &parcel : {reply, data}) {
        auto released = releaseParcelObjects(*parcel, parcel->storage->Bytes);
        if (released.stop != ExecutionStop::Returned)
          return released;
        if (parcel->release)
          parcel->release();
        parcel->release = {};
        heap.resizeBytes(*parcel->storage, 0);
      }
      return completed;
    };
    return result;
  };
  if (!found->second.alive())
    return respond(-74); // attemptIncStrong could not retain the target.
  if (incoming.code == 0x5f504e47) { // Native BBinder::PING_TRANSACTION.
    if (!heap.resizeBytes(*reply->storage, 4))
      return {ExecutionStop::HeapLimit,
              0,
              {},
              "Android Binder ping reply budget exhausted"};
    return respond(0);
  }
  ++found->second.references;
  const auto object = found->second.object;
  // BinderCallsStats' default (non-detailed) tracking uses a duration of one,
  // but its singleton still records System.currentTimeMillis at construction.
  if (!binder_calls_start) {
    if (!services.realtime_milliseconds)
      return unsupported(
          "Android BinderCallsStats realtime clock is not supplied");
    auto now = services.realtime_milliseconds();
    if (now.stop != ExecutionStop::Returned)
      return now;
    binder_calls_start = now.value;
  }
  auto wrapped_data = wrapNativeParcel(heap, data, admit);
  if (wrapped_data.stop != ExecutionStop::Returned)
    return wrapped_data;
  auto wrapped_reply = wrapNativeParcel(heap, reply, admit);
  if (wrapped_reply.stop != ExecutionStop::Returned)
    return wrapped_reply;
  if (binder_tracing_enabled)
    return unsupported(
        "Android incoming Binder tracing requires a Java stack trace");
  auto policy = binderThreadPolicy();
  if (policy.stop != ExecutionStop::Returned)
    return policy;
  ExecutionResult result{ExecutionStop::Suspended};
  result.external.emplace();
  result.external->kind = ExternalCall::JavaMethod;
  result.external->java_method =
      MethodRef{"Landroid/os/Binder;",
                "onTransact",
                {"I", "Landroid/os/Parcel;", "Landroid/os/Parcel;", "I"},
                "Z"};
  result.external->arguments = {object, incoming.code, wrapped_data.value,
                                wrapped_reply.value, incoming.flags};
  auto finish = [this, &heap, &program, admit, incoming = std::move(incoming),
                 policy = policy.value, source = wrapped_data.value,
                 destination = wrapped_reply.value,
                 respond = std::move(respond)](ExecutionResult completed) {
    const auto *recycle =
        libraryMethod({"Landroid/os/Parcel;", "recycle", {}, "V"}, false);
    for (auto wrapper : {destination, source}) {
      auto recycled = executeLibrary(heap, *recycle, wrapper, {}, admit, false,
                                     &program, this);
      if (recycled.stop != ExecutionStop::Returned)
        return recycled;
    }
    // No supported StrictMode path creates gathered violations; the initial
    // ThreadLocal null remains null after clearGatheredViolations.
    if (!binder_calls.contains(incoming.uid) &&
        !heap.reserveBytes(
            binder_calls_storage,
            (binder_calls.size() + 1) *
                ::neverd::limits::dalvik::kFrameworkFieldStateOverheadBytes))
      return ExecutionResult{
          ExecutionStop::HeapLimit,
          0,
          {},
          "Android BinderCallsStats storage budget exhausted"};
    ++binder_calls[incoming.uid];
    auto current = binderThreadPolicy();
    if (current.stop != ExecutionStop::Returned)
      return current;
    if (current.value != policy)
      return ExecutionResult{ExecutionStop::Unsupported,
                             0,
                             {},
                             "Android incoming Binder policy restoration "
                             "requires Java BlockGuard state"};
    if (incoming.code == 0x5f535052)
      return ExecutionResult{
          ExecutionStop::Unsupported,
          0,
          {},
          "Android Binder system property callbacks are not modeled"};
    --local_binders.find(incoming.cookie)->second.references;
    auto released = retireLocalBinder(incoming.cookie);
    if (released.stop != ExecutionStop::Returned)
      return released;
    return respond(completed.value ? 0 : -74);
  };
  result.external->complete = [this, &heap, &program, admit,
                               destination = wrapped_reply.value,
                               finish = std::move(finish)](
                                  ExecutionResult completed) {
    if (completed.stop == ExecutionStop::Returned)
      return finish(std::move(completed));
    auto uncaught = [](ExecutionResult error) {
      return ExecutionResult{ExecutionStop::Unsupported,
                             0,
                             {},
                             "Android JavaBBinder uncaught exception logging "
                             "is not modeled: " +
                                 error.exception};
    };
    const auto type = "L" + completed.exception + ";";
    if (!program.assignable(type, "Ljava/lang/RuntimeException;") &&
        !program.assignable(type, "Landroid/os/RemoteException;"))
      return uncaught(std::move(completed));
    // LOG_RUNTIME_EXCEPTION starts false; modifying it is not yet modeled.
    // execTransact rewinds without truncating the partially written reply.
    const auto *seek = libraryMethod(
        {"Landroid/os/Parcel;", "setDataPosition", {"I"}, "V"}, false);
    const uint64_t zero[] = {0};
    auto rewound = executeLibrary(heap, *seek, destination, zero, admit, false,
                                  &program, this);
    if (rewound.stop != ExecutionStop::Returned)
      return rewound;
    ExecutionResult serialized{ExecutionStop::Suspended};
    serialized.external.emplace();
    serialized.external->kind = ExternalCall::JavaMethod;
    serialized.external->java_method = MethodRef{"Landroid/os/Parcel;",
                                                 "writeException",
                                                 {"Ljava/lang/Exception;"},
                                                 "V"};
    serialized.external->arguments = {destination, completed.throwable};
    serialized.external->complete = [finish,
                                     uncaught](ExecutionResult written) {
      if (written.stop != ExecutionStop::Returned)
        return uncaught(std::move(written));
      written.value = 1; // A serialized exception is a handled transaction.
      return finish(std::move(written));
    };
    return serialized;
  };
  return result;
}
} // namespace neverd::mobile::dalvik
