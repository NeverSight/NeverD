//===- AndroidBinder.cpp - Shared API 28 Binder transaction entry --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Ordered Java wrapper and native Parcel checks before Binder transport.
//===----------------------------------------------------------------------===//

#include "AndroidFramework.h"

#include <cstdint>
#include <span>

namespace neverd::mobile::dalvik {
ExecutionResult
AndroidFramework::binderTransact(Heap &heap, const Program &program,
                                 const LibraryMethod &method, uint64_t receiver,
                                 std::span<const uint64_t> arguments,
                                 const AdmitInstruction &admit) {
  // Local Binder.transact rewinds Parcels and dispatches onTransact. It must
  // not acquire BinderProxy semantics merely through the IBinder interface.
  if (heap.get(receiver)->ClassName != "android/os/BinderProxy")
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android local Binder onTransact dispatch is not modeled"};
  if (method.Op == BinderTransact) {
    // API 28 CHECK_PARCEL_SIZE is false. There is no size read or null check
    // before the warning/tracing wrapper. FLAG_ONEWAY suppresses the warning
    // independently of the otherwise unknown mWarnOnBlocking value.
    if (!(uint32_t(arguments[3]) & 1)) {
      const auto *warning = program.findField(
          {"Landroid/os/BinderProxy;", "mWarnOnBlocking", "Z"}, false);
      auto enabled = field(heap, *warning, receiver, {}, admit);
      if (enabled.stop != ExecutionStop::Returned)
        return enabled;
      if (enabled.value) {
        field(heap, *warning, receiver, 0, admit);
        return {ExecutionStop::Unsupported,
                0,
                {},
                "Android Binder blocking warning requires Log.w and a Java "
                "stack trace"};
      }
    }
    if (binder_tracing_enabled)
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Android Binder transaction tracing requires a Java stack trace"};
  }
  if (!arguments[1])
    return {ExecutionStop::Thrown, 0, "java/lang/NullPointerException"};
  auto data = parcelStatus(arguments[1]);
  if (data.stop != ExecutionStop::Returned)
    return data;
  if (arguments[2]) {
    auto reply = parcelStatus(arguments[2]);
    if (reply.stop != ExecutionStop::Returned)
      return reply;
  }
  if (!binder_warnings.contains(receiver))
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android BinderProxy native object is not supplied"};
  // The retained Java key and warning flag establish no BpBinder handle,
  // ProcessState connection, liveness or reply. Keep that boundary explicit.
  return {ExecutionStop::Unsupported,
          0,
          {},
          "Android BinderProxy.transactNative requires an explicit native "
          "Binder binding"};
}
} // namespace neverd::mobile::dalvik
