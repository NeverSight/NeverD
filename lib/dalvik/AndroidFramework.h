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
  AndroidFrameworkState inspect(const Heap &heap, const Program &program) const;
  std::optional<AndroidApplicationSnapshot::ClassLookupFailure::Loader>
  definingLoader(std::string_view type, const Program &program) const;

private:
  ExecutionResult binderThreadPolicy();
  ExecutionResult parcel(Heap &heap, const LibraryMethod &method,
                         uint64_t receiver, std::span<const uint64_t> arguments,
                         const AdmitInstruction &admit);
  struct ParcelState {
    uint32_t position = 0;
    bool recycled = false;
  };
  std::map<uint32_t, ParcelState> parcels;
  // API 28 owned-Parcel pool, in first-empty/first-occupied slot order.
  std::array<uint32_t, 6> parcel_pool{};
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
  ExecutionResult serviceProxy(Heap &heap, const Program &program,
                               uint64_t binder, const AdmitInstruction &admit);
  std::map<const Field *, std::optional<uint64_t>> service_fields;
  std::map<uint32_t, uint32_t> binder_identities;
  std::map<uint32_t, std::optional<bool>> binder_warnings;
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
