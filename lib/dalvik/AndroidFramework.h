//===- AndroidFramework.h - Framework object identities -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared framework objects, retained queue links and lifecycle inputs.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DALVIK_ANDROID_FRAMEWORK_H
#define NEVERD_DALVIK_ANDROID_FRAMEWORK_H

#include "neverd/dalvik/Library.h"

#include "llvm/ADT/DenseMap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string_view>

namespace neverd::mobile::dalvik {
class AndroidFramework {
public:
  AndroidFramework(std::optional<AndroidApplicationSnapshot> input,
                   const Program &program, RuntimeServices services = {});
  ExecutionResult execute(Heap &heap, const Program &program,
                          const LibraryMethod &method, uint64_t receiver,
                          std::span<const uint64_t> arguments,
                          const AdmitInstruction &admit);
  ExecutionResult field(Heap &heap, const Field &field, uint64_t receiver,
                        std::optional<uint64_t> write,
                        const AdmitInstruction &admit);
  ExecutionResult initializeServiceManager(
      Heap &heap, const Program &program, const AdmitInstruction &admit,
      const std::function<ExecutionResult(std::string_view)> &initialize);
  // Library owns URL values and selects the already-validated scheme.
  ExecutionResult openURL(Heap &heap, std::string_view scheme,
                          const AdmitInstruction &admit);
  ExecutionResult completeBinder(Heap &heap, const ExternalCall &call,
                                 BinderReply reply);
  ExecutionResult binderReference(BinderReferenceOperation operation,
                                  uint64_t pointer, uint64_t cookie);
  ExecutionResult dispatchBinder(Heap &heap, const Program &program,
                                 BinderIncoming incoming,
                                 const AdmitInstruction &admit);
  AndroidFrameworkState inspect(const Heap &heap, const Program &program) const;
  std::optional<AndroidApplicationSnapshot::ClassLookupFailure::Loader>
  definingLoader(std::string_view type, const Program &program) const;

private:
  ExecutionResult constructBinder(Heap &heap, const Program &program,
                                  uint64_t receiver,
                                  const AdmitInstruction &admit);
  ExecutionResult localBinder(Heap &heap, const Program &program,
                              uint64_t receiver);
  ExecutionResult retireLocalBinder(uint64_t cookie);
  struct BinderHolder {
    uint64_t address = 0, cookie = 0;
  };
  struct LocalBinder {
    uint32_t object = 0;
    uint64_t pointer = 0, references = 0;
    uint64_t strong = 0, weak = 0;
    // waitForResponse queues dereferences. Only the separate top-level
    // polling/thread-pool dispatcher drains them, not a synchronous call.
    uint64_t pending_strong = 0, pending_weak = 0;
    bool alive() const { return references || strong; }
  };
  llvm::DenseMap<uint32_t, BinderHolder> binder_holders;
  llvm::DenseMap<uint64_t, LocalBinder> local_binders;
  ExecutionResult binderThreadPolicy();
  ExecutionResult binderTransact(Heap &heap, const Program &program,
                                 const LibraryMethod &method, uint64_t receiver,
                                 std::span<const uint64_t> arguments,
                                 const AdmitInstruction &admit);
  ExecutionResult binderInterface(Heap &heap, const Program &program,
                                  const LibraryMethod &method,
                                  uint64_t receiver,
                                  std::span<const uint64_t> arguments,
                                  const AdmitInstruction &admit);
  // API 28 Binder's new class state; also exposed by sTracingEnabled.
  bool binder_tracing_enabled = false;
  ExecutionResult parcelStatus(uint64_t receiver) const;
  ExecutionResult readParcelBuffer(Heap &heap, uint64_t receiver);
  ExecutionResult parcel(Heap &heap, const Program &program,
                         const LibraryMethod &method, uint64_t receiver,
                         std::span<const uint64_t> arguments,
                         const AdmitInstruction &admit);
  ExecutionResult parcelException(Heap &heap, const Program &program,
                                  uint64_t receiver, uint64_t exception,
                                  const AdmitInstruction &admit);
  struct ParcelState {
    // Non-owned Java wrappers have their own pool and refer to a native
    // Parcel whose lifetime spans reply acknowledgement, not Java recycle.
    std::shared_ptr<ParcelState> native;
    std::unique_ptr<Object> storage;
    uint32_t position = 0;
    uint32_t capacity = 0;
    bool recycled = false;
    int32_t error = 0;
    std::function<ExecutionResult(std::span<const uint8_t> &)> view;
    std::function<void()> release;
    std::function<ExecutionResult(uint64_t, std::span<const uint8_t>)> write;
    std::function<ExecutionResult(uint64_t)> reallocate_objects;
    std::vector<uint64_t> objects;
    // Owned native Parcels release the current records at these offsets, not
    // the identities originally written there. Borrowed buffers belong to IPC.
    bool owns_objects = true;
    bool objects_sorted = false;
    size_t object_hint = 0;
    std::optional<uint32_t> fd;
  };
  ExecutionResult releaseParcelObjects(ParcelState &state,
                                       std::span<const uint8_t> bytes,
                                       bool reverse = true);
  ExecutionResult acquireParcelObjects(ParcelState &state,
                                       std::span<const uint8_t> bytes);
  ExecutionResult retainBinderHandle(uint32_t fd, uint32_t handle);
  llvm::DenseMap<std::pair<uint32_t, uint32_t>, uint64_t> binder_parcel_handles;
  std::map<uint32_t, ParcelState> parcels;
  // API 28 owned-Parcel pool, in first-empty/first-occupied slot order.
  std::array<uint32_t, 6> parcel_pool{};
  std::array<uint32_t, 6> native_parcel_pool{};
  ParcelState &parcelState(uint64_t receiver);
  Object &parcelStorage(Heap &heap, uint64_t receiver);
  ExecutionResult wrapNativeParcel(Heap &heap,
                                   std::shared_ptr<ParcelState> state,
                                   const AdmitInstruction &admit);
  ExecutionResult replaceParcel(Heap &heap, ParcelState &state, Object &object,
                                BinderReply reply);
  std::optional<uint64_t> binder_calls_start;
  std::map<uint32_t, uint64_t> binder_calls; // Default tracking: time == count.
  Object binder_calls_storage{};
  ExecutionResult eventLog(Heap &heap, const Program &program,
                           const LibraryMethod &method,
                           std::span<const uint64_t> arguments,
                           const AdmitInstruction &admit);
  ExecutionResult serviceManager(Heap &heap, const Program &program,
                                 const LibraryMethod &method, uint64_t receiver,
                                 std::span<const uint64_t> arguments,
                                 const AdmitInstruction &admit);
  ExecutionResult binderProxy(Heap &heap, uint32_t key,
                              const AdmitInstruction &admit);
  ExecutionResult binderHandle(Heap &heap, uint32_t fd, uint32_t handle,
                               const AdmitInstruction &admit);
  ExecutionResult serviceProxy(Heap &heap, const Program &program,
                               uint64_t binder, const AdmitInstruction &admit);
  std::map<const Field *, std::optional<uint64_t>> service_fields;
  std::map<uint32_t, uint32_t> binder_identities;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> binder_handles;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> binder_input_keys;
  std::map<uint32_t, std::optional<bool>> binder_warnings;
  std::map<
      uint32_t,
      AndroidApplicationSnapshot::ServiceManager::BinderProxy::NativeBinding>
      binder_bindings;
  bool service_manager_initialized = false;
  ExecutionResult recordStatistic(Heap &heap, const Program &program,
                                  uint64_t receiver, int32_t event,
                                  uint64_t start,
                                  const AdmitInstruction &admit);
  ExecutionResult readClock(LibraryOperation operation);
  ExecutionResult nextMessage(Heap &heap, const Program &program,
                              uint64_t receiver, const AdmitInstruction &admit);
  ExecutionResult post(Heap &heap, const Program &program, uint64_t receiver,
                       uint64_t callback, const AdmitInstruction &admit);
  ExecutionResult stringValue(Heap &heap, std::string_view text,
                              const AdmitInstruction &admit);
  ExecutionResult stringArrayValue(
      Heap &heap,
      const AndroidApplicationSnapshot::ApplicationInfo::SplitSourceDirectories
          &values,
      const AdmitInstruction &admit);
  enum class ApplicationInfoField {
    SourceDirectory,
    NativeLibraryDirectory,
    NonLocalizedLabel,
    TargetSdkVersion,
    SplitSourceDirectories,
    SplitPublicSourceDirectories
  };
  enum class ApplicationInfoSource { Context, ApplicationQuery, PackageQuery };
  ExecutionResult createApplicationInfo(Heap &heap,
                                        ApplicationInfoSource source,
                                        size_t query,
                                        const AdmitInstruction &admit);
  ExecutionResult applicationInfoFieldValue(Heap &heap, uint64_t receiver,
                                            ApplicationInfoField selected,
                                            std::optional<uint64_t> write,
                                            const AdmitInstruction &admit);
  ExecutionResult instrumentationValue(Heap &heap, uint64_t receiver,
                                       std::optional<uint64_t> write,
                                       const AdmitInstruction &admit);
  ExecutionResult packageInfoFieldValue(Heap &heap, uint64_t receiver,
                                        std::string_view field,
                                        std::optional<uint64_t> write,
                                        const AdmitInstruction &admit);
  std::optional<AndroidApplicationSnapshot> snapshot;
  RuntimeServices services;
  std::optional<uint64_t> instrumentation;
  uint32_t thread = 0, application = 0, context = 0;
  uint32_t boot_loader = 0, system_loader = 0, app_loader = 0, package = 0;
  uint32_t files_directory = 0, files_path = 0;
  uint32_t package_resource_path = 0;
  uint32_t content_resolver = 0;
  uint32_t package_manager = 0;
  uint32_t secure_android_id = 0, global_adb_enabled = 0;
  uint32_t application_info = 0;
  uint32_t default_locale = 0, locale_language = 0, locale_country = 0;
  uint32_t main_looper = 0;
  uint32_t main_queue = 0, message_head = 0;
  bool main_queue_blocked = false;
  struct ApplicationInfoState {
    // Query results refer to immutable input by source/index, so framework
    // moves do not invalidate pointers or alias separate parcel objects.
    ApplicationInfoSource source = ApplicationInfoSource::Context;
    size_t query = 0;
    std::optional<uint64_t> source_directory;
    std::optional<uint64_t> native_library_directory;
    std::optional<uint64_t> non_localized_label;
    std::optional<uint64_t> target_sdk_version;
    std::optional<uint64_t> split_source_directories;
    std::optional<uint64_t> split_public_source_directories;
  };
  std::map<uint64_t, ApplicationInfoState> application_info_states;
  struct PackageInfoState {
    size_t query = 0;
    std::optional<uint64_t> package_name;
    std::optional<uint64_t> version_name;
    std::optional<uint64_t> version_code;
    std::optional<uint64_t> application_info;
  };
  std::map<uint64_t, PackageInfoState> package_info_states;
};
} // namespace neverd::mobile::dalvik

#endif // NEVERD_DALVIK_ANDROID_FRAMEWORK_H
