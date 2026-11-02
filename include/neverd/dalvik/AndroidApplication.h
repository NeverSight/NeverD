//===- AndroidApplication.h - Explicit framework process state --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::mobile::dalvik {
// An API 28 snapshot, not execution of ActivityThread.bindApplication. The
// application phase contains the platform Application attached to a ContextImpl
// and a single PathClassLoader for supplied DEX declarations, with boot parent.
// Custom Application subclasses, split loaders and constructors need separate
// evidence/execution; this input does not infer them from native libraries.
struct AndroidApplicationSnapshot {
  enum class Phase {
    Unattached,
    Attached,
    Application
  } phase = Phase::Unattached;
  // Unknown when absent. Only meaningful in the application phase.
  std::optional<std::string> package_name;
  // Initialized, existing app-private files directory. No mkdir/chmod is
  // inferred. Native workloads also require the same directory in LinuxFiles.
  std::optional<std::string> files_directory;
  // Independent of application creation: binding may install instrumentation
  // first, including a custom subclass. Absence is unknown, not null/default.
  enum class Instrumentation { Uninitialized, Platform };
  std::optional<Instrumentation> instrumentation;
  // Explicit negative observations for exactly one loader and query string.
  // They neither close a classpath nor apply to JNI FindClass. A null message
  // declares a null Throwable message, rather than an inferred path string.
  struct ClassLookupFailure {
    enum class Loader {
      Unspecified,
      Boot,
      System,
      Application
    } loader = Loader::Unspecified;
    std::string binary_name;
    std::optional<std::string> message;
    // Alternative to a strict binary name. Preserve the raw printable ASCII
    // query, including whitespace; binary_name must be empty when supplied.
    std::optional<std::string> exact_query;
  };
  std::vector<ClassLookupFailure> class_lookup_failures;
  // A LoadedApk can establish its application loader before constructing the
  // Application. Absent is unknown in Attached; the other phases imply false
  // and true respectively. Explicit contradictory values are rejected.
  std::optional<bool> application_loader_established;
  // A populated, generation-stable API 28 NameValueCache for this resolver's
  // own process user. This does not model provider acquisition or Binder I/O.
  // Missing snapshot is unknown; a present null value is a cached absence.
  struct SecureSettingsCache {
    std::optional<std::string> android_id;
  };
  std::optional<SecureSettingsCache> secure_settings_cache;
  // Separate Global NameValueCache state for the deprecated Secure key.
  // The same own-user, populated and stable-generation preconditions apply.
  struct GlobalSettingsCache {
    std::optional<std::string> adb_enabled;
  };
  std::optional<GlobalSettingsCache> global_settings_cache;
  // Fields of the attached Context's retained ApplicationInfo. No APK bytes
  // or filesystem entry are inferred from the Java String. Missing input is
  // unknown; an explicitly null source_dir is a known null field value.
  struct ApplicationInfo {
    std::optional<std::string> source_dir;
    // Missing outer optional is unknown. Present inner null is a known null
    // nativeLibraryDir field, independent of sourceDir and actual file bytes.
    std::optional<std::optional<std::string>> native_library_dir;
    // Optional String-backed initial nonLocalizedLabel. Guest writes may
    // retain any compatible CharSequence; absence does not imply null.
    std::optional<std::optional<std::string>> non_localized_label;
    // Independent Java int field; not inferred from the platform SDK property.
    std::optional<int32_t> target_sdk_version;
    // Unknown, known null, or an ordered String array. Values do not imply
    // split APK files exist. Array and element references are mutable.
    using SplitSourceDirectories = std::vector<std::optional<std::string>>;
    std::optional<std::optional<SplitSourceDirectories>> split_source_dirs;
    // Independently supplied resource paths. No alias or fallback to the
    // private split paths is inferred, even when the contents are equal.
    std::optional<std::optional<SplitSourceDirectories>>
        split_public_source_dirs;
  };
  std::optional<ApplicationInfo> application_info;
  // Stable availability of the package service for this Context. Unknown when
  // absent; false makes getPackageManager return null. True permits one cached
  // ApplicationPackageManager identity, not any package query or Binder I/O.
  std::optional<bool> package_manager_available;
  // Positive API 28 package-service responses for this Context's own user,
  // after ApplicationPackageManager's ABI path adjustment. Exact package/flag
  // keys only; absence is unknown, not NameNotFoundException. Each invocation
  // returns a separate parcel result with independent mutable fields.
  struct ApplicationInfoQuery {
    std::string package_name;
    int32_t flags = 0;
    ApplicationInfo info;
  };
  std::vector<ApplicationInfoQuery> application_info_queries;
  // Explicit stable PackageItemInfo static state, not its assumed initializer.
  // True requires safe-label processing, which this projection does not model.
  std::optional<bool> package_item_force_safe_labels;
  // Stable LoadedApk.mResDir for this attached Application. Unknown when the
  // outer optional is absent; inner null is an explicit null Java value.
  // Independent of mutable ApplicationInfo fields and filesystem contents.
  std::optional<std::optional<std::string>> package_resource_path;
  // Partial positive PackageInfo responses for exact own-user package/flag
  // queries. Missing fields remain unknown, rather than Java default values.
  struct PackageInfo {
    std::optional<std::optional<std::string>> package_name;
    std::optional<std::optional<std::string>> version_name;
    std::optional<int32_t> version_code;
    // Absent is unknown; an engaged null is a known null applicationInfo.
    // Each parcel result owns a distinct mutable ApplicationInfo when supplied.
    std::optional<std::optional<ApplicationInfo>> application_info;
  };
  struct PackageInfoQuery {
    std::string package_name;
    int32_t flags = 0;
    PackageInfo info;
  };
  std::vector<PackageInfoQuery> package_info_queries;
  // Stable process default Locale, independent of application attachment.
  // Components are already-normalized API 28 BaseLocale values, not inputs
  // to a constructor. Absent object/components are unknown; empty is known.
  struct Locale {
    std::optional<std::string> language;
    std::optional<std::string> country;
  };
  std::optional<Locale> default_locale;
  // Stable API 28 DexPathList fields of the retained PathClassLoader. Every
  // admitted DEX element has a nonnull DexFile and nonnull ZIP File path, so
  // its toString does not depend on a filesystem isDirectory observation.
  // These are File.getPath values, not a request to open/load any DEX or ELF.
  struct ClassLoaderPaths {
    std::vector<std::string> dex_zip_files;
    std::vector<std::string> native_library_directories;
    std::vector<std::string> system_native_library_directories;
  };
  std::optional<ClassLoaderPaths> application_loader_paths;
  std::optional<ClassLoaderPaths> system_loader_paths;
  // Stable sMainLooper state, independent of Application attachment. Absent
  // is unknown, false is known null, true retains one Looper and its queue.
  // This supplies identities only, not thread execution or queue contents.
  std::optional<bool> main_looper_prepared;
  // Initial state of the prepared main queue: empty and not quitting. The
  // blocked flag is independent of emptiness; waking a blocked queue requires
  // a native event-loop service. Absence leaves queue contents unknown.
  struct EmptyMessageQueue {
    bool blocked = false;
    // True explicitly excludes native callbacks/timers and descriptor
    // listeners from a nonblocking nativePollOnce. Missing is unknown;
    // false retains an unsupported native event-loop boundary.
    std::optional<bool> native_callbacks_empty;
  };
  std::optional<EmptyMessageQueue> initial_main_queue;
  // Message.obtain's process-wide pool. Only an explicitly empty pool can
  // allocate a fresh Message; false preserves an unmodeled nonempty pool.
  std::optional<bool> initial_message_pool_empty;
  // Java-side state at the workload boundary, independent of attachment.
  // Proxy keys identify retained Java objects, NOT kernel handles or peers.
  // No transaction response, liveness or native Binder ownership is implied.
  struct ServiceManager {
    struct BinderProxy {
      uint32_t key = 0;
      std::optional<bool> warn_on_blocking;
    };
    std::vector<BinderProxy> binder_proxies;
    struct CacheEntry {
      std::optional<std::string> name;
      // Null is a cached null; a positive key refers to binder_proxies.
      std::optional<uint32_t> binder;
    };
    // Absent is unknown; an empty vector is an explicitly empty whole cache.
    std::optional<std::vector<CacheEntry>> cache;
    // Absent is unknown, inner null is known null. A key in manager_proxy
    // declares a ServiceManagerProxy retaining that BinderProxy; context_object
    // is the independently observed BinderInternal.getContextObject result.
    std::optional<std::optional<uint32_t>> manager_proxy;
    std::optional<std::optional<uint32_t>> context_object;
    // Stable, successful semantic RPC replies for exact manager/name/method
    // queries. No missing entry implies absence. Binder keys retain the same
    // Java identities as the cache; this does not model native Parcel I/O.
    struct LookupResult {
      enum class Operation { Get, Check } operation = Operation::Get;
      uint32_t manager = 0;
      std::optional<std::string> name;
      std::optional<uint32_t> binder;
    };
    std::vector<LookupResult> lookup_results;
  };
  std::optional<ServiceManager> service_manager;
  // An explicitly installed custom URLStreamHandler, supplied as initial host
  // state. Its openConnection always throws a fresh IOException with this
  // retained message. This is not the default HTTP handler or a network policy.
  // No entry for a scheme means unknown, not success or a simulated denial.
  struct URLHandlerFailure {
    std::string scheme;
    std::optional<std::string> message;
  };
  std::vector<URLHandlerFailure> url_open_failure_handlers;
};
std::optional<bool>
androidApplicationLoaderState(const AndroidApplicationSnapshot &input);
// Shared structural admission for typed callers and the JSON adapter.
// Declaration conflicts additionally require the Runtime's actual Program.
void validateAndroidClassLookupFailures(
    const AndroidApplicationSnapshot &input);
void validateAndroidSecureSettingsCache(
    const AndroidApplicationSnapshot &input);
void validateAndroidGlobalSettingsCache(
    const AndroidApplicationSnapshot &input);
void validateAndroidApplicationInfo(const AndroidApplicationSnapshot &input);
void validateAndroidApplicationInfoQueries(
    const AndroidApplicationSnapshot &input);
void validateAndroidPackageResourcePath(
    const AndroidApplicationSnapshot &input);
void validateAndroidPackageInfoQueries(const AndroidApplicationSnapshot &input);
void validateAndroidDefaultLocale(const AndroidApplicationSnapshot &input);
void validateAndroidClassLoaderPaths(const AndroidApplicationSnapshot &input);
void validateAndroidMessageQueue(const AndroidApplicationSnapshot &input);
void validateAndroidServiceManager(const AndroidApplicationSnapshot &input);
void validateAndroidURLHandlers(const AndroidApplicationSnapshot &input);
} // namespace neverd::mobile::dalvik
