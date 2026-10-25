//===- AndroidFramework.cpp - Explicit API 28 application projection ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared JNI/DEX identities over a declared lifecycle snapshot. This does not
/// execute ART startup, application constructors or arbitrary loader code.
//===----------------------------------------------------------------------===//
#include "AndroidFramework.h"

#include "neverd/dalvik/Limits.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd::mobile::dalvik {
namespace {
const std::string &
classLookupName(const AndroidApplicationSnapshot::ClassLookupFailure &failure) {
  return failure.exact_query ? *failure.exact_query : failure.binary_name;
}
bool platformType(std::string_view type) {
  return type.starts_with("Ljava/") || type.starts_with("Ljavax/") ||
         type.starts_with("Landroid/") || type.starts_with("Ldalvik/");
}
void validateSettingsCacheValue(const AndroidApplicationSnapshot &input,
                                const std::optional<std::string> &value,
                                const char *scope) {
  const std::string prefix = std::string("Android ") + scope;
  if (input.phase != AndroidApplicationSnapshot::Phase::Application)
    throw Error(prefix + " settings cache requires an attached Application");
  if (value) {
    if (value->size() > ::neverd::limits::dalvik::kMaxAndroidSettingsValueBytes)
      throw Error(prefix + " setting value exceeds its budget");
    (void)javaStringBytes(*value);
  }
}
void validatePackageName(const std::string &name) {
  if (name.empty() ||
      name.size() > ::neverd::limits::dalvik::kMaxAndroidClassNameBytes ||
      name.find('\0') != std::string::npos)
    throw Error("invalid Android application package snapshot");
  (void)javaStringBytes(name);
}
void validateApplicationInfoFields(
    const AndroidApplicationSnapshot::ApplicationInfo &info) {
  auto check = [](const std::optional<std::string> &value, const char *name) {
    if (!value)
      return;
    if (value->size() >
        ::neverd::limits::dalvik::kMaxAndroidApplicationInfoFieldBytes)
      throw Error(std::string("Android ApplicationInfo ") + name +
                  " exceeds its budget");
    (void)javaStringBytes(*value);
  };
  check(info.source_dir, "sourceDir");
  if (info.native_library_dir)
    check(*info.native_library_dir, "nativeLibraryDir");
  if (info.non_localized_label)
    check(*info.non_localized_label, "nonLocalizedLabel");
  for (auto [name, field] :
       {std::pair{"splitSourceDirs", &info.split_source_dirs},
        {"splitPublicSourceDirs", &info.split_public_source_dirs}}) {
    if (!*field || !**field)
      continue;
    const auto &values = ***field;
    if (values.size() >
        ::neverd::limits::dalvik::kMaxAndroidApplicationSplitSources)
      throw Error(std::string("Android ApplicationInfo ") + name +
                  " exceeds its count budget");
    for (const auto &value : values)
      check(value, name);
  }
}
void validatePackageInfoFields(
    const AndroidApplicationSnapshot::PackageInfo &info) {
  for (const auto *field : {&info.package_name, &info.version_name})
    if (*field && **field) {
      const auto &text = ***field;
      if (text.size() >
          ::neverd::limits::dalvik::kMaxAndroidPackageInfoFieldBytes)
        throw Error("Android PackageInfo String exceeds its budget");
      (void)javaStringBytes(text);
    }
  if (info.application_info && *info.application_info)
    validateApplicationInfoFields(**info.application_info);
}
template <class Query, class Validate>
void validatePackageQueries(const AndroidApplicationSnapshot &input,
                            const std::vector<Query> &queries,
                            std::string_view kind, size_t maximum,
                            Validate validate) {
  if (queries.empty())
    return;
  const auto prefix = "Android " + std::string(kind);
  if (input.phase != AndroidApplicationSnapshot::Phase::Application ||
      input.package_manager_available != true)
    throw Error(prefix + " queries require an available package service and "
                         "attached Application");
  if (queries.size() > maximum)
    throw Error(prefix + " query inventory exceeds its budget");
  std::set<std::pair<std::string, int32_t>> keys;
  for (const auto &query : queries) {
    validatePackageName(query.package_name);
    if (!keys.emplace(query.package_name, query.flags).second)
      throw Error("duplicate " + prefix + " query");
    validate(query.info);
  }
}
} // namespace

std::optional<bool>
androidApplicationLoaderState(const AndroidApplicationSnapshot &input) {
  using Phase = AndroidApplicationSnapshot::Phase;
  if ((input.phase == Phase::Unattached &&
       input.application_loader_established == true) ||
      (input.phase == Phase::Application &&
       input.application_loader_established == false))
    throw Error("Android application loader contradicts lifecycle phase");
  if (input.phase == Phase::Unattached)
    return false;
  if (input.phase == Phase::Application)
    return true;
  return input.application_loader_established;
}

void validateAndroidClassLookupFailures(
    const AndroidApplicationSnapshot &input) {
  using Loader = AndroidApplicationSnapshot::ClassLookupFailure::Loader;
  auto loader_state = androidApplicationLoaderState(input);
  if (input.class_lookup_failures.size() >
      ::neverd::limits::dalvik::kMaxAndroidApplicationClassLookupFailures)
    throw Error("Android class lookup failure inventory exceeds its budget");
  std::set<std::pair<Loader, std::string>> seen;
  for (const auto &failure : input.class_lookup_failures) {
    if ((failure.loader != Loader::Boot && failure.loader != Loader::System &&
         failure.loader != Loader::Application) ||
        (failure.loader == Loader::Application && loader_state != true))
      throw Error("invalid Android class lookup failure loader");
    const auto &name = classLookupName(failure);
    if (name.empty() ||
        name.size() > ::neverd::limits::dalvik::kMaxAndroidClassNameBytes)
      throw Error("invalid Android class lookup failure name");
    if (failure.exact_query) {
      if (!failure.binary_name.empty() || name.front() == '[' ||
          std::any_of(name.begin(), name.end(), [](unsigned char byte) {
            return byte < 0x20 || byte > 0x7e || byte == '/';
          }))
        throw Error("invalid Android class lookup failure exact query");
    } else {
      bool first = true;
      for (unsigned char byte : name) {
        if (byte == '.' && !first) {
          first = true;
          continue;
        }
        if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
              byte == '_' || byte == '$' ||
              (!first && byte >= '0' && byte <= '9')))
          throw Error("invalid Android class lookup failure binary name");
        first = false;
      }
      if (first)
        throw Error("invalid Android class lookup failure binary name");
    }
    if (!seen.emplace(failure.loader, name).second)
      throw Error("duplicate Android class lookup failure");
    if (failure.message) {
      if (failure.message->size() >
          ::neverd::limits::dalvik::kMaxAndroidLookupMessageBytes)
        throw Error("Android class lookup failure message exceeds its budget");
      (void)javaStringBytes(*failure.message);
    }
  }
}

void validateAndroidSecureSettingsCache(
    const AndroidApplicationSnapshot &input) {
  if (!input.secure_settings_cache)
    return;
  validateSettingsCacheValue(input, input.secure_settings_cache->android_id,
                             "secure");
}
void validateAndroidGlobalSettingsCache(
    const AndroidApplicationSnapshot &input) {
  if (input.global_settings_cache)
    validateSettingsCacheValue(input, input.global_settings_cache->adb_enabled,
                               "global");
}
void validateAndroidApplicationInfo(const AndroidApplicationSnapshot &input) {
  if (!input.application_info)
    return;
  if (input.phase != AndroidApplicationSnapshot::Phase::Application)
    throw Error("Android ApplicationInfo requires an attached Application");
  validateApplicationInfoFields(*input.application_info);
}

void validateAndroidApplicationInfoQueries(
    const AndroidApplicationSnapshot &input) {
  validatePackageQueries(
      input, input.application_info_queries, "application info",
      ::neverd::limits::dalvik::kMaxAndroidApplicationInfoQueries,
      validateApplicationInfoFields);
}
void validateAndroidPackageInfoQueries(
    const AndroidApplicationSnapshot &input) {
  validatePackageQueries(
      input, input.package_info_queries, "package info",
      ::neverd::limits::dalvik::kMaxAndroidPackageInfoQueries,
      validatePackageInfoFields);
}

void validateAndroidPackageResourcePath(
    const AndroidApplicationSnapshot &input) {
  if (!input.package_resource_path)
    return;
  if (input.phase != AndroidApplicationSnapshot::Phase::Application)
    throw Error(
        "Android package resource path requires an attached Application");
  if (const auto &text = *input.package_resource_path) {
    if (text->size() >
        ::neverd::limits::dalvik::kMaxAndroidPackageResourcePathBytes)
      throw Error("Android package resource path exceeds its budget");
    (void)javaStringBytes(*text);
  }
}

void validateAndroidDefaultLocale(const AndroidApplicationSnapshot &input) {
  if (!input.default_locale)
    return;
  const auto &locale = *input.default_locale;
  for (const auto *component : {&locale.language, &locale.country}) {
    if (!*component)
      continue;
    const auto &text = **component;
    if (text.size() > ::neverd::limits::dalvik::kMaxAndroidLocaleComponentBytes)
      throw Error("Android default Locale component exceeds its budget");
    (void)javaStringBytes(text);
    const bool language = component == &locale.language;
    for (unsigned char byte : text)
      if ((language && byte >= 'A' && byte <= 'Z') ||
          (!language && byte >= 'a' && byte <= 'z'))
        throw Error("Android default Locale component is not normalized");
    if (language && (text == "he" || text == "yi" || text == "id"))
      throw Error(
          "Android default Locale requires API 28 legacy language codes");
  }
}

void validateAndroidClassLoaderPaths(const AndroidApplicationSnapshot &input) {
  if (input.application_loader_paths &&
      androidApplicationLoaderState(input) != true)
    throw Error(
        "Android application loader paths require an established loader");
  for (const auto *state :
       {&input.application_loader_paths, &input.system_loader_paths}) {
    if (!*state)
      continue;
    const auto &paths = **state;
    size_t remaining =
        ::neverd::limits::dalvik::kMaxAndroidClassLoaderPathTotalBytes;
    for (const auto *list :
         {&paths.dex_zip_files, &paths.native_library_directories,
          &paths.system_native_library_directories}) {
      if (list->size() > ::neverd::limits::dalvik::kMaxAndroidClassLoaderPaths)
        throw Error("Android class loader path inventory exceeds its budget");
      for (const auto &path : *list) {
        if (path.size() >
                ::neverd::limits::dalvik::kMaxAndroidClassLoaderPathBytes ||
            path.size() > remaining)
          throw Error("Android class loader paths exceed their byte budget");
        (void)javaStringBytes(path);
        remaining -= path.size();
      }
    }
  }
}

void validateAndroidMessageQueue(const AndroidApplicationSnapshot &input) {
  if (input.initial_main_queue && input.main_looper_prepared != true)
    throw Error("Android initial main queue requires a prepared main Looper");
}

void validateAndroidURLHandlers(const AndroidApplicationSnapshot &input) {
  if (input.url_open_failure_handlers.size() >
      ::neverd::limits::dalvik::kMaxAndroidURLFailureHandlers)
    throw Error("Android URL handler inventory exceeds its budget");
  std::set<std::string> schemes;
  for (const auto &handler : input.url_open_failure_handlers) {
    if ((handler.scheme != "http" && handler.scheme != "https") ||
        !schemes.insert(handler.scheme).second)
      throw Error("Android URL failure handler requires a unique HTTP scheme");
    if (handler.message) {
      if (handler.message->size() >
          ::neverd::limits::dalvik::kMaxAndroidURLHandlerMessageBytes)
        throw Error("Android URL handler message exceeds its byte budget");
      (void)javaStringBytes(*handler.message);
    }
  }
}

ExecutionResult AndroidFramework::openURL(Heap &heap, std::string_view scheme,
                                          const AdmitInstruction &admit) {
  const AndroidApplicationSnapshot::URLHandlerFailure *selected = nullptr;
  if (snapshot)
    for (const auto &handler : snapshot->url_open_failure_handlers)
      if (handler.scheme == scheme)
        selected = &handler;
  if (!selected)
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Java URL.openConnection handler state is not supplied"};
  auto bytes = selected->message ? javaStringBytes(*selected->message)
                                 : std::vector<uint8_t>{};
  // Charge all work before allocating or exposing a message/throwable. A model
  // resource stop never becomes a catchable Java exception.
  for (size_t i = 0; i < bytes.size() / 2 + 1; ++i) {
    auto stop = admit();
    if (stop != ExecutionStop::Returned)
      return {stop, 0, {}, "Java URL handler work budget exhausted"};
  }
  uint32_t message = 0;
  if (selected->message) {
    auto value = heap.stringConstant("android.url_handler." + selected->scheme +
                                         ":" + *selected->message,
                                     std::move(bytes));
    if (!value)
      return {ExecutionStop::HeapLimit,
              0,
              {},
              "Java URL handler message storage budget exhausted"};
    message = value->Identity;
  }
  auto exception = heap.allocate(Object::Throwable, "java/io/IOException", 0);
  if (!exception)
    return {ExecutionStop::HeapLimit,
            0,
            {},
            "Java URL handler throwable storage budget exhausted"};
  exception->Message = message;
  return {ExecutionStop::Thrown, 0, "java/io/IOException",
          "explicit custom URL handler failure", exception->Identity};
}

void Program::addAndroidFramework() {
  // Only the hierarchy needed by this projection. Unmodeled members are not
  // supplied as empty executable declarations.
  for (auto [name, base] :
       {std::pair{"android/app/ClientTransactionHandler", "java/lang/Object"},
        {"android/app/ActivityThread", "android/app/ClientTransactionHandler"},
        {"android/app/Instrumentation", "java/lang/Object"},
        {"android/content/ComponentCallbacks", "java/lang/Object"},
        {"android/content/ComponentCallbacks2", "java/lang/Object"},
        {"android/content/Context", "java/lang/Object"},
        {"android/content/ContextWrapper", "android/content/Context"},
        {"android/app/Application", "android/content/ContextWrapper"},
        {"android/app/ContextImpl", "android/content/Context"},
        {"android/content/pm/PackageItemInfo", "java/lang/Object"},
        {"android/content/pm/ApplicationInfo",
         "android/content/pm/PackageItemInfo"},
        {"android/content/pm/PackageInfo", "java/lang/Object"},
        {"android/os/Parcelable", "java/lang/Object"},
        {"android/os/Handler", "java/lang/Object"},
        {"android/os/Looper", "java/lang/Object"},
        {"android/os/MessageQueue", "java/lang/Object"},
        {"android/os/Message", "java/lang/Object"},
        {"java/lang/Runnable", "java/lang/Object"},
        {"android/content/ContentResolver", "java/lang/Object"},
        {"android/app/ContextImpl$ApplicationContentResolver",
         "android/content/ContentResolver"},
        {"android/provider/BaseColumns", "java/lang/Object"},
        {"android/provider/Settings$NameValueTable", "java/lang/Object"},
        {"android/provider/Settings$Secure",
         "android/provider/Settings$NameValueTable"},
        {"java/io/File", "java/lang/Object"},
        {"java/net/URL", "java/lang/Object"},
        {"java/net/URLConnection", "java/lang/Object"},
        {"java/io/Serializable", "java/lang/Object"},
        {"java/lang/Cloneable", "java/lang/Object"},
        {"java/util/Locale", "java/lang/Object"},
        {"java/lang/Comparable", "java/lang/Object"},
        {"java/lang/CharSequence", "java/lang/Object"},
        {"java/lang/Appendable", "java/lang/Object"},
        {"java/lang/AbstractStringBuilder", "java/lang/Object"},
        {"java/lang/StringBuilder", "java/lang/AbstractStringBuilder"},
        {"java/lang/ClassLoader", "java/lang/Object"},
        {"java/lang/BootClassLoader", "java/lang/ClassLoader"},
        {"dalvik/system/BaseDexClassLoader", "java/lang/ClassLoader"},
        {"dalvik/system/PathClassLoader", "dalvik/system/BaseDexClassLoader"},
        {"android/content/pm/PackageManager", "java/lang/Object"},
        {"android/app/ApplicationPackageManager",
         "android/content/pm/PackageManager"}}) {
    Class type;
    type.name = "L" + std::string(name) + ";";
    type.superclass = "L" + std::string(base) + ";";
    type.access = {"public"};
    if (std::string_view(name) == "java/net/URLConnection" ||
        std::string_view(name) == "android/content/Context" ||
        std::string_view(name) == "android/content/ContentResolver" ||
        std::string_view(name) == "android/content/pm/PackageManager" ||
        std::string_view(name) == "java/lang/AbstractStringBuilder" ||
        std::string_view(name) == "java/lang/ClassLoader" ||
        std::string_view(name) == "android/app/ClientTransactionHandler")
      type.access.insert("abstract");
    if (std::string_view(name) == "android/app/ActivityThread" ||
        std::string_view(name) == "android/provider/Settings$Secure" ||
        std::string_view(name) == "java/lang/StringBuilder" ||
        std::string_view(name) == "java/util/Locale" ||
        std::string_view(name) == "java/net/URL" ||
        std::string_view(name) == "android/os/Looper" ||
        std::string_view(name) == "android/os/MessageQueue" ||
        std::string_view(name) == "android/os/Message" ||
        std::string_view(name) ==
            "android/app/ContextImpl$ApplicationContentResolver")
      type.access.insert("final");
    if (std::string_view(name) == "android/app/ContextImpl" ||
        std::string_view(name) == "java/lang/AbstractStringBuilder" ||
        std::string_view(name) ==
            "android/app/ContextImpl$ApplicationContentResolver" ||
        std::string_view(name) == "java/lang/BootClassLoader")
      type.access.erase("public");
    if (std::string_view(name) == "android/content/ComponentCallbacks" ||
        std::string_view(name) == "android/os/Parcelable" ||
        std::string_view(name) == "android/provider/BaseColumns" ||
        std::string_view(name) == "android/content/ComponentCallbacks2" ||
        std::string_view(name) == "java/io/Serializable" ||
        std::string_view(name) == "java/lang/Cloneable" ||
        std::string_view(name) == "java/lang/Runnable" ||
        std::string_view(name) == "java/lang/CharSequence" ||
        std::string_view(name) == "java/lang/Appendable" ||
        std::string_view(name) == "java/lang/Comparable")
      type.access.insert({"abstract", "interface"});
    if (std::string_view(name) == "android/content/ComponentCallbacks2")
      type.interfaces.push_back("Landroid/content/ComponentCallbacks;");
    if (std::string_view(name) == "android/app/Application")
      type.interfaces.push_back("Landroid/content/ComponentCallbacks2;");
    if (std::string_view(name) == "android/content/pm/ApplicationInfo" ||
        std::string_view(name) == "android/content/pm/PackageInfo" ||
        std::string_view(name) == "android/os/Message")
      type.interfaces.push_back("Landroid/os/Parcelable;");
    if (std::string_view(name) == "java/io/File")
      type.interfaces = {"Ljava/io/Serializable;", "Ljava/lang/Comparable;"};
    if (std::string_view(name) == "java/net/URL")
      type.interfaces = {"Ljava/io/Serializable;"};
    if (std::string_view(name) == "java/util/Locale")
      type.interfaces = {"Ljava/lang/Cloneable;", "Ljava/io/Serializable;"};
    if (std::string_view(name) == "java/lang/AbstractStringBuilder")
      type.interfaces = {"Ljava/lang/Appendable;", "Ljava/lang/CharSequence;"};
    if (std::string_view(name) == "java/lang/StringBuilder")
      type.interfaces = {"Ljava/io/Serializable;", "Ljava/lang/CharSequence;"};
    if (std::string_view(name) == "android/provider/Settings$NameValueTable")
      type.interfaces = {"Landroid/provider/BaseColumns;"};
    type.source_id = "android-api28-framework";
    if (std::string_view(name) == "android/os/Message") {
      for (auto [field_name, field_type] :
           {std::pair{"what", "I"},
            {"arg1", "I"},
            {"arg2", "I"},
            {"obj", "Ljava/lang/Object;"},
            {"replyTo", "Landroid/os/Messenger;"},
            {"sendingUid", "I"},
            {"flags", "I"},
            {"when", "J"},
            {"data", "Landroid/os/Bundle;"},
            {"target", "Landroid/os/Handler;"},
            {"callback", "Ljava/lang/Runnable;"},
            {"next", "Landroid/os/Message;"}}) {
        Field field;
        field.reference = {type.name, field_name, field_type};
        if (std::string_view(field_name) == "what" ||
            std::string_view(field_name) == "arg1" ||
            std::string_view(field_name) == "arg2" ||
            std::string_view(field_name) == "obj" ||
            std::string_view(field_name) == "replyTo" ||
            std::string_view(field_name) == "sendingUid")
          field.access.insert("public");
        type.fields.push_back(std::move(field));
      }
    }
    if (std::string_view(name) == "java/lang/Runnable") {
      Method run;
      run.reference = {type.name, "run", {}, "V"};
      run.access = {"public", "abstract"};
      type.methods.push_back(std::move(run));
    }
    if (std::string_view(name) == "android/os/Handler" ||
        std::string_view(name) == "android/os/Looper") {
      for (auto [field_name, field_type] :
           {std::pair{"mLooper", "Landroid/os/Looper;"},
            {"mQueue", "Landroid/os/MessageQueue;"},
            {"mCallback", "Landroid/os/Handler$Callback;"},
            {"mAsynchronous", "Z"}}) {
        if (std::string_view(name) == "android/os/Looper" &&
            std::string_view(field_name) != "mQueue")
          continue;
        Field field;
        field.reference = {type.name, field_name, field_type};
        field.access = {"final"};
        type.fields.push_back(std::move(field));
      }
    }
    if (std::string_view(name) == "android/app/ActivityThread") {
      Field field;
      field.reference = {type.name, "mInstrumentation",
                         "Landroid/app/Instrumentation;"};
      type.fields.push_back(std::move(field));
    }
    if (std::string_view(name) == "android/provider/Settings$Secure") {
      // API 28 compile-time keys, independent of settings-provider state.
      // Runtime owns their ordinary static storage and literal identities.
      for (auto [key, value] : {std::pair{"ADB_ENABLED", "adb_enabled"},
                                {"ANDROID_ID", "android_id"}}) {
        Field field;
        field.reference = {type.name, key, "Ljava/lang/String;"};
        field.access = {"public", "static", "final"};
        field.value = std::string(value);
        type.fields.push_back(std::move(field));
      }
    }
    if (std::string_view(name) == "android/content/pm/PackageItemInfo") {
      Field field;
      field.reference = {type.name, "nonLocalizedLabel",
                         "Ljava/lang/CharSequence;"};
      field.access = {"public"};
      type.fields.push_back(std::move(field));
    }
    if (std::string_view(name) == "android/content/pm/ApplicationInfo") {
      for (const char *field_name :
           {"sourceDir", "nativeLibraryDir", "targetSdkVersion",
            "splitSourceDirs", "splitPublicSourceDirs"}) {
        Field field;
        field.reference = {
            type.name, field_name,
            (std::string_view(field_name) == "splitSourceDirs" ||
             std::string_view(field_name) == "splitPublicSourceDirs")
                ? "[Ljava/lang/String;"
            : std::string_view(field_name) == "targetSdkVersion"
                ? "I"
                : "Ljava/lang/String;"};
        field.access = {"public"};
        type.fields.push_back(std::move(field));
      }
    }
    if (std::string_view(name) == "android/content/pm/PackageInfo") {
      for (const char *field_name :
           {"packageName", "versionName", "versionCode", "applicationInfo"}) {
        Field field;
        field.reference = {type.name, field_name,
                           std::string_view(field_name) == "applicationInfo"
                               ? "Landroid/content/pm/ApplicationInfo;"
                           : std::string_view(field_name) == "versionCode"
                               ? "I"
                               : "Ljava/lang/String;"};
        field.access = {"public"};
        type.fields.push_back(std::move(field));
      }
    }
    auto [entry, inserted] = classes.emplace(type.name, std::move(type));
    if (inserted &&
        (std::string_view(name) == "android/app/ActivityThread" ||
         std::string_view(name) == "android/content/pm/PackageItemInfo" ||
         std::string_view(name) == "android/content/pm/ApplicationInfo" ||
         std::string_view(name) == "android/content/pm/PackageInfo"))
      for (const auto &field : entry->second.fields)
        android_framework_fields.insert(&field);
  }
}
bool Program::androidFrameworkField(const Field &field) const {
  return android_framework_fields.contains(&field);
}

AndroidFramework::AndroidFramework(
    std::optional<AndroidApplicationSnapshot> input, const Program &program,
    RuntimeServices services)
    : snapshot(std::move(input)), services(std::move(services)) {
  if (!snapshot)
    return;
  validateAndroidClassLookupFailures(*snapshot);
  validateAndroidSecureSettingsCache(*snapshot);
  validateAndroidGlobalSettingsCache(*snapshot);
  validateAndroidApplicationInfo(*snapshot);
  validateAndroidApplicationInfoQueries(*snapshot);
  validateAndroidPackageInfoQueries(*snapshot);
  validateAndroidPackageResourcePath(*snapshot);
  validateAndroidDefaultLocale(*snapshot);
  validateAndroidClassLoaderPaths(*snapshot);
  validateAndroidMessageQueue(*snapshot);
  if (snapshot->initial_main_queue)
    main_queue_blocked = snapshot->initial_main_queue->blocked;
  validateAndroidURLHandlers(*snapshot);
  for (const auto &failure : snapshot->class_lookup_failures) {
    std::string type = "L" + classLookupName(failure) + ";";
    std::replace(type.begin(), type.end(), '.', '/');
    if (program.knownReference(type) &&
        (platformType(type) || failure.loader ==
                                   AndroidApplicationSnapshot::
                                       ClassLookupFailure::Loader::Application))
      throw Error("Android class lookup failure shadows a visible declaration");
  }
  using Phase = AndroidApplicationSnapshot::Phase;
  if (snapshot->phase != Phase::Unattached &&
      snapshot->phase != Phase::Attached &&
      snapshot->phase != Phase::Application)
    throw Error("invalid Android application phase");
  if (snapshot->instrumentation &&
      (snapshot->phase == Phase::Unattached ||
       (*snapshot->instrumentation !=
            AndroidApplicationSnapshot::Instrumentation::Uninitialized &&
        *snapshot->instrumentation !=
            AndroidApplicationSnapshot::Instrumentation::Platform)))
    throw Error("invalid Android instrumentation snapshot");
  if (snapshot->package_name) {
    if (snapshot->phase != Phase::Application)
      throw Error("invalid Android application package snapshot");
    validatePackageName(*snapshot->package_name);
  }
  if (snapshot->files_directory) {
    auto &path = *snapshot->files_directory;
    if (snapshot->phase != Phase::Application || path.empty() ||
        path.front() != '/' ||
        path.size() >
            ::neverd::limits::dalvik::kMaxAndroidSharedLibraryPathBytes ||
        path.find('\0') != std::string::npos)
      throw Error("invalid Android files directory snapshot");
    (void)javaStringBytes(path);
    // File's Unix lexical normalization only. Dot components are not resolved
    // and no host filesystem or user.dir property participates.
    std::string normalized;
    for (char byte : path)
      if (byte != '/' || normalized.empty() || normalized.back() != '/')
        normalized.push_back(byte);
    if (normalized.size() > 1 && normalized.back() == '/')
      normalized.pop_back();
    path = std::move(normalized);
  }
}

ExecutionResult AndroidFramework::stringValue(Heap &heap, std::string_view text,
                                              const AdmitInstruction &admit) {
  auto bytes = javaStringBytes(text);
  for (size_t i = 0; i < bytes.size(); i += 2) {
    auto budget = admit();
    if (budget != ExecutionStop::Returned)
      return {budget, 0, {}, "Android framework work budget exhausted"};
  }
  auto object = heap.allocate(Object::String, "java/lang/String", bytes.size());
  if (!object)
    return {ExecutionStop::HeapLimit,
            0,
            {},
            "Android framework storage budget exhausted"};
  object->Bytes = std::move(bytes);
  object->Length = object->Bytes.size() / 2;
  return {ExecutionStop::Returned, object->Identity, {}, {}};
}

ExecutionResult AndroidFramework::stringArrayValue(
    Heap &heap,
    const AndroidApplicationSnapshot::ApplicationInfo::SplitSourceDirectories
        &values,
    const AdmitInstruction &admit) {
  auto array = heap.allocate(Object::ObjectArray, "[Ljava/lang/String;", 0,
                             values.size() * sizeof(std::weak_ptr<Object>));
  if (!array)
    return {ExecutionStop::HeapLimit,
            0,
            {},
            "Android framework storage budget exhausted"};
  array->Length = values.size();
  array->ElementSize = sizeof(std::weak_ptr<Object>);
  array->Elements.resize(values.size());
  for (size_t i = 0; i != values.size(); ++i) {
    auto budget = admit();
    if (budget != ExecutionStop::Returned)
      return {budget, 0, {}, "Android framework work budget exhausted"};
    if (!values[i])
      continue;
    auto value = stringValue(heap, *values[i], admit);
    if (value.stop != ExecutionStop::Returned)
      return value;
    array->Elements[i] = heap.get(value.value);
  }
  return {ExecutionStop::Returned, array->Identity, {}, {}};
}

ExecutionResult AndroidFramework::createApplicationInfo(
    Heap &heap, ApplicationInfoSource source, size_t query,
    const AdmitInstruction &admit) {
  auto budget = admit();
  if (budget != ExecutionStop::Returned)
    return {budget, 0, {}, "Android framework work budget exhausted"};
  auto object = heap.allocate(
      Object::Instance, "android/content/pm/ApplicationInfo", 0,
      sizeof(ApplicationInfoState) +
          ::neverd::limits::dalvik::kFrameworkFieldStateOverheadBytes);
  if (!object)
    return {ExecutionStop::HeapLimit,
            0,
            {},
            "Android framework storage budget exhausted"};
  ApplicationInfoState state;
  state.source = source;
  state.query = query;
  application_info_states.emplace(object->Identity, std::move(state));
  return {ExecutionStop::Returned, object->Identity, {}, {}};
}

ExecutionResult AndroidFramework::applicationInfoFieldValue(
    Heap &heap, uint64_t receiver, ApplicationInfoField selected,
    std::optional<uint64_t> write, const AdmitInstruction &admit) {
  const auto found = application_info_states.find(receiver);
  if (!snapshot || found == application_info_states.end())
    return {ExecutionStop::Unsupported,
            0,
            {},
            "unmodeled Android ApplicationInfo instance"};
  auto budget = admit();
  if (budget != ExecutionStop::Returned)
    return {budget, 0, {}, "Android framework work budget exhausted"};
  auto &state = found->second;
  auto &retained =
      selected == ApplicationInfoField::NativeLibraryDirectory
          ? state.native_library_directory
      : selected == ApplicationInfoField::NonLocalizedLabel
          ? state.non_localized_label
      : selected == ApplicationInfoField::TargetSdkVersion
          ? state.target_sdk_version
      : selected == ApplicationInfoField::SplitSourceDirectories
          ? state.split_source_directories
      : selected == ApplicationInfoField::SplitPublicSourceDirectories
          ? state.split_public_source_directories
          : state.source_directory;
  if (write)
    retained = *write;
  if (!retained) {
    const std::optional<std::string> *text = nullptr;
    const auto *info =
        state.source == ApplicationInfoSource::ApplicationQuery
            ? &snapshot->application_info_queries[state.query].info
        : state.source == ApplicationInfoSource::PackageQuery
            ? &**snapshot->package_info_queries[state.query]
                     .info.application_info
        : snapshot->application_info ? &*snapshot->application_info
                                     : nullptr;
    if (selected == ApplicationInfoField::SplitSourceDirectories ||
        selected == ApplicationInfoField::SplitPublicSourceDirectories) {
      bool public_sources =
          selected == ApplicationInfoField::SplitPublicSourceDirectories;
      const auto *initial = !info            ? nullptr
                            : public_sources ? &info->split_public_source_dirs
                                             : &info->split_source_dirs;
      if (!initial || !*initial)
        return {
            ExecutionStop::Unsupported,
            0,
            {},
            public_sources
                ? "Android ApplicationInfo splitPublicSourceDirs is not "
                  "supplied"
                : "Android ApplicationInfo splitSourceDirs is not supplied"};
      if (!**initial)
        retained = 0;
      else {
        auto value = stringArrayValue(heap, ***initial, admit);
        if (value.stop != ExecutionStop::Returned)
          return value;
        retained = value.value;
      }
      return {ExecutionStop::Returned, *retained, {}, {}};
    }
    if (selected == ApplicationInfoField::TargetSdkVersion) {
      if (!info || !info->target_sdk_version)
        return {ExecutionStop::Unsupported,
                0,
                {},
                "Android ApplicationInfo targetSdkVersion is not supplied"};
      retained = uint32_t(*info->target_sdk_version);
      return {ExecutionStop::Returned, *retained, {}, {}};
    }
    if (info) {
      if (selected == ApplicationInfoField::SourceDirectory)
        text = &info->source_dir;
      else if (selected == ApplicationInfoField::NativeLibraryDirectory &&
               info->native_library_dir)
        text = &*info->native_library_dir;
      else if (selected == ApplicationInfoField::NonLocalizedLabel &&
               info->non_localized_label)
        text = &*info->non_localized_label;
    }
    if (!text)
      return {ExecutionStop::Unsupported,
              0,
              {},
              selected == ApplicationInfoField::NonLocalizedLabel
                  ? "Android ApplicationInfo nonLocalizedLabel is not supplied"
              : selected == ApplicationInfoField::NativeLibraryDirectory
                  ? "Android ApplicationInfo nativeLibraryDir is not supplied"
                  : "Android ApplicationInfo sourceDir is not supplied"};
    if (!*text) {
      retained = 0;
    } else {
      auto value = stringValue(heap, **text, admit);
      if (value.stop != ExecutionStop::Returned)
        return value;
      retained = value.value;
    }
  }
  return {ExecutionStop::Returned, *retained, {}, {}};
}

ExecutionResult AndroidFramework::packageInfoFieldValue(
    Heap &heap, uint64_t receiver, std::string_view field,
    std::optional<uint64_t> write, const AdmitInstruction &admit) {
  const auto found = package_info_states.find(receiver);
  if (!snapshot || found == package_info_states.end())
    return {ExecutionStop::Unsupported,
            0,
            {},
            "unmodeled Android PackageInfo instance"};
  auto budget = admit();
  if (budget != ExecutionStop::Returned)
    return {budget, 0, {}, "Android framework work budget exhausted"};
  auto &state = found->second;
  auto &retained = field == "applicationInfo" ? state.application_info
                   : field == "packageName"   ? state.package_name
                   : field == "versionName"   ? state.version_name
                                              : state.version_code;
  if (write)
    retained = *write; // Runtime owns primitive narrowing and reference checks.
  if (!retained) {
    const auto &info = snapshot->package_info_queries[state.query].info;
    auto unknown = [&]() -> ExecutionResult {
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Android PackageInfo " + std::string(field) + " is not supplied"};
    };
    if (field == "applicationInfo") {
      if (!info.application_info)
        return unknown();
      if (!*info.application_info)
        retained = 0;
      else {
        auto value = createApplicationInfo(
            heap, ApplicationInfoSource::PackageQuery, state.query, admit);
        if (value.stop != ExecutionStop::Returned)
          return value;
        retained = value.value;
      }
    } else if (field == "versionCode") {
      if (!info.version_code)
        return unknown();
      retained = uint32_t(*info.version_code);
    } else {
      const auto &text =
          field == "packageName" ? info.package_name : info.version_name;
      if (!text)
        return unknown();
      if (!*text)
        retained = 0;
      else {
        auto value = stringValue(heap, **text, admit);
        if (value.stop != ExecutionStop::Returned)
          return value;
        retained = value.value;
      }
    }
  }
  return {ExecutionStop::Returned, *retained, {}, {}};
}

ExecutionResult
AndroidFramework::instrumentationValue(Heap &heap, uint64_t receiver,
                                       std::optional<uint64_t> write,
                                       const AdmitInstruction &admit) {
  if (!snapshot || !thread || receiver != thread)
    return {ExecutionStop::Unsupported,
            0,
            {},
            "unmodeled Android ActivityThread instance"};
  auto budget = admit();
  if (budget != ExecutionStop::Returned)
    return {budget, 0, {}, "Android framework work budget exhausted"};
  if (write)
    instrumentation = *write;
  if (!instrumentation) {
    if (!snapshot->instrumentation)
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Android instrumentation state is not supplied"};
    if (*snapshot->instrumentation ==
        AndroidApplicationSnapshot::Instrumentation::Uninitialized) {
      instrumentation = 0;
    } else {
      auto value =
          heap.allocate(Object::Instance, "android/app/Instrumentation", 0);
      if (!value)
        return {ExecutionStop::HeapLimit,
                0,
                {},
                "Android framework storage budget exhausted"};
      instrumentation = value->Identity;
    }
  }
  return {ExecutionStop::Returned, *instrumentation, {}, {}};
}

ExecutionResult AndroidFramework::field(Heap &heap, const Field &field,
                                        uint64_t receiver,
                                        std::optional<uint64_t> write,
                                        const AdmitInstruction &admit) {
  const auto &ref = field.reference;
  if (ref.owner == "Landroid/app/ActivityThread;" &&
      ref.name == "mInstrumentation" &&
      ref.type == "Landroid/app/Instrumentation;" &&
      !has(field.access, "static"))
    return instrumentationValue(heap, receiver, write, admit);
  if (ref.owner == "Landroid/content/pm/ApplicationInfo;" &&
      (ref.name == "sourceDir" || ref.name == "nativeLibraryDir") &&
      ref.type == "Ljava/lang/String;" && !has(field.access, "static"))
    return applicationInfoFieldValue(
        heap, receiver,
        ref.name == "nativeLibraryDir"
            ? ApplicationInfoField::NativeLibraryDirectory
            : ApplicationInfoField::SourceDirectory,
        write, admit);
  if (ref.owner == "Landroid/content/pm/PackageItemInfo;" &&
      ref.name == "nonLocalizedLabel" &&
      ref.type == "Ljava/lang/CharSequence;" && !has(field.access, "static"))
    return applicationInfoFieldValue(
        heap, receiver, ApplicationInfoField::NonLocalizedLabel, write, admit);
  if (ref.owner == "Landroid/content/pm/ApplicationInfo;" &&
      ref.name == "targetSdkVersion" && ref.type == "I" &&
      !has(field.access, "static"))
    return applicationInfoFieldValue(
        heap, receiver, ApplicationInfoField::TargetSdkVersion, write, admit);
  if (ref.owner == "Landroid/content/pm/ApplicationInfo;" &&
      (ref.name == "splitSourceDirs" || ref.name == "splitPublicSourceDirs") &&
      ref.type == "[Ljava/lang/String;" && !has(field.access, "static"))
    return applicationInfoFieldValue(
        heap, receiver,
        ref.name == "splitPublicSourceDirs"
            ? ApplicationInfoField::SplitPublicSourceDirectories
            : ApplicationInfoField::SplitSourceDirectories,
        write, admit);
  if (ref.owner == "Landroid/content/pm/PackageInfo;" &&
      ((ref.name == "applicationInfo" &&
        ref.type == "Landroid/content/pm/ApplicationInfo;") ||
       (ref.name == "versionCode" && ref.type == "I") ||
       ((ref.name == "packageName" || ref.name == "versionName") &&
        ref.type == "Ljava/lang/String;")) &&
      !has(field.access, "static"))
    return packageInfoFieldValue(heap, receiver, ref.name, write, admit);
  return {ExecutionStop::Unsupported,
          0,
          {},
          "Android framework field is not modeled"};
}

AndroidFrameworkState AndroidFramework::inspect(const Heap &heap,
                                                const Program &program) const {
  AndroidFrameworkState result;
  if (snapshot) {
    result.declared_phase = snapshot->phase;
    result.application_loader_established =
        androidApplicationLoaderState(*snapshot);
  }
  result.activity_thread = thread;
  result.application = application;
  result.context = context;
  result.boot_loader = boot_loader;
  result.system_loader = system_loader;
  result.application_loader = app_loader;
  result.instrumentation = instrumentation;
  if (main_queue && snapshot->initial_main_queue) {
    auto &queue = result.main_message_queue.emplace();
    queue.identity = main_queue;
    queue.blocked = main_queue_blocked;
    std::set<uint64_t> seen;
    uint64_t current = message_head;
    while (current) {
      auto message = heap.get(current);
      if (!message || message->ClassName != "android/os/Message" ||
          !seen.insert(current).second ||
          seen.size() > ::neverd::limits::dalvik::kMaxQueuedMessages) {
        queue.complete = false;
        break;
      }
      auto value = [&](const char *name, const char *type) {
        auto field =
            program.findField({"Landroid/os/Message;", name, type}, false);
        auto entry = message->Fields.find(field);
        if (entry == message->Fields.end()) {
          queue.complete = false;
          return uint64_t(0);
        }
        return entry->second;
      };
      AndroidMessageState row;
      row.identity = message->Identity;
      row.target = value("target", "Landroid/os/Handler;");
      row.callback = value("callback", "Ljava/lang/Runnable;");
      row.next = value("next", "Landroid/os/Message;");
      row.when = std::bit_cast<int64_t>(value("when", "J"));
      row.flags = uint32_t(value("flags", "I"));
      queue.messages.push_back(row);
      if (!queue.complete)
        break;
      current = row.next;
    }
  }
  return result;
}

ExecutionResult AndroidFramework::post(Heap &heap, const Program &program,
                                       uint64_t receiver, uint64_t callback,
                                       const AdmitInstruction &admit) {
  struct Stop {
    ExecutionResult result;
  };
  auto fail = [](ExecutionStop reason, const char *message) -> void {
    throw Stop{{reason, 0, {}, message}};
  };
  auto tick = [&] {
    if (auto reason = admit(); reason != ExecutionStop::Returned)
      fail(reason, "Android message work budget exhausted");
  };
  auto field = [&](const char *owner, const char *name, const char *type) {
    auto result = program.findField({owner, name, type}, false);
    if (!result)
      fail(ExecutionStop::Unsupported, "Android message declaration is absent");
    return result;
  };
  auto read = [&](const std::shared_ptr<Object> &object, const Field *entry) {
    if (!object || !object->Fields.contains(entry))
      fail(ExecutionStop::Unsupported,
           "Android message field storage is unknown");
    return object->Fields.at(entry);
  };
  try {
    if (!snapshot || !snapshot->initial_message_pool_empty)
      fail(ExecutionStop::Unsupported,
           "Android Message pool state is not supplied");
    if (!*snapshot->initial_message_pool_empty)
      fail(ExecutionStop::Unsupported,
           "Android nonempty Message pool is unmodeled");
    // Only the explicitly empty pool reaches Message's real zeroed allocation
    // and sendingUid initializer. No recycled object's previous data is
    // guessed.
    tick();
    const auto &fields = program.findClass("Landroid/os/Message;")->fields;
    auto message = heap.allocate(
        Object::Instance, "android/os/Message", 0,
        fields.size() * ::neverd::limits::dalvik::kInstanceFieldStorageBytes);
    if (!message)
      fail(ExecutionStop::HeapLimit,
           "Android Message storage budget exhausted");
    for (const auto &entry : fields)
      message->Fields.emplace(&entry, 0);
    auto member = [&](const char *name, const char *type) {
      return field("Landroid/os/Message;", name, type);
    };
    message->Fields.at(member("sendingUid", "I")) = UINT32_MAX;
    message->Fields.at(member("callback", "Ljava/lang/Runnable;")) = callback;
    tick();
    if (!services.uptime_milliseconds)
      fail(ExecutionStop::Unsupported,
           "Android uptime service is not supplied");
    auto time = services.uptime_milliseconds();
    if (time.stop != ExecutionStop::Returned)
      return time;
    if (time.value > INT64_MAX)
      fail(ExecutionStop::Unsupported,
           "Android uptime is outside the modeled range");
    auto handler = heap.get(receiver);
    if (auto reason = program.unsupportedClass(objectDescriptor(*handler)))
      fail(ExecutionStop::Unsupported, reason->c_str());
    // post is final, but its sendMessageAtTime call is virtual. Preserve that
    // boundary instead of selecting the platform body for a supplied override.
    const MethodRef send{"Landroid/os/Handler;",
                         "sendMessageAtTime",
                         {"Landroid/os/Message;", "J"},
                         "Z"};
    if (program.virtualMethod(send, objectDescriptor(*handler)))
      fail(ExecutionStop::Unsupported,
           "Android Handler.sendMessageAtTime override requires execution");
    const auto queue = read(handler, field("Landroid/os/Handler;", "mQueue",
                                           "Landroid/os/MessageQueue;"));
    if (!queue)
      fail(ExecutionStop::Unsupported,
           "Android Handler null-queue warning requires logging");
    if (queue != main_queue || !snapshot->initial_main_queue)
      fail(ExecutionStop::Unsupported,
           "Android Handler queue state is not supplied");
    const auto asynchronous =
        read(handler, field("Landroid/os/Handler;", "mAsynchronous", "Z"));
    auto next_field = member("next", "Landroid/os/Message;");
    auto when_field = member("when", "J");
    auto target_field = member("target", "Landroid/os/Handler;");
    auto flags_field = member("flags", "I");
    // Traverse the retained heap list, not a second copied queue. Every link
    // and ordering decision is bounded and charged before queue mutation.
    std::vector<std::shared_ptr<Object>> pending;
    std::set<uint64_t> seen;
    uint64_t current = message_head;
    while (current) {
      tick();
      if (!seen.insert(current).second)
        fail(ExecutionStop::Invalid, "Android Message queue contains a cycle");
      if (pending.size() >= ::neverd::limits::dalvik::kMaxQueuedMessages - 1)
        fail(ExecutionStop::HeapLimit, "Android Message queue limit exhausted");
      auto item = heap.get(current);
      if (!item || !item->Initialized ||
          item->ClassName != "android/os/Message")
        fail(ExecutionStop::Invalid,
             "Android Message queue contains an invalid link");
      (void)read(item, when_field);
      (void)read(item, target_field);
      (void)read(item, flags_field);
      current = read(item, next_field);
      pending.push_back(std::move(item));
    }
    size_t at = 0;
    if (time.value)
      while (at < pending.size() &&
             int64_t(time.value) >=
                 std::bit_cast<int64_t>(pending[at]->Fields.at(when_field)))
        ++at;
    bool wake = main_queue_blocked && at == 0;
    if (main_queue_blocked && at && asynchronous &&
        !pending.front()->Fields.at(target_field)) {
      wake = true;
      for (size_t i = 1; i < at; ++i)
        if (pending[i]->Fields.at(flags_field) & 2)
          wake = false;
    }
    tick();
    message->Fields.at(target_field) = receiver;
    message->Fields.at(flags_field) = asynchronous ? 3 : 1;
    message->Fields.at(when_field) = time.value;
    message->Fields.at(next_field) =
        at < pending.size() ? pending[at]->Identity : 0;
    if (at)
      pending[at - 1]->Fields.at(next_field) = message->Identity;
    else
      message_head = message->Identity;
    // API 28 enqueues before nativeWake. Preserve the partial state on this
    // unsupported path; neither wake nor Runnable dispatch is fabricated.
    if (wake)
      fail(ExecutionStop::Unsupported,
           "Android MessageQueue.nativeWake is unmodeled");
    return {ExecutionStop::Returned, 1};
  } catch (Stop &stopped) {
    return std::move(stopped.result);
  }
}

ExecutionResult AndroidFramework::nextMessage(Heap &heap,
                                              const Program &program,
                                              uint64_t receiver,
                                              const AdmitInstruction &admit) {
  // API 28 MessageQueue.next first polls with timeout zero. The explicit
  // native-callback input admits only its no-callback path. Empty/future
  // queues still require blocking native polling and are refused below.
  if (!snapshot || !snapshot->initial_main_queue || !main_queue ||
      receiver != main_queue)
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android MessageQueue state is not supplied"};
  if (snapshot->initial_main_queue->native_callbacks_empty != true)
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android MessageQueue native callback state is not empty or known"};
  struct Stop {
    ExecutionResult result;
  };
  auto fail = [](ExecutionStop reason, const char *message) -> void {
    throw Stop{{reason, 0, {}, message}};
  };
  auto tick = [&] {
    if (auto reason = admit(); reason != ExecutionStop::Returned)
      fail(reason, "Android message work budget exhausted");
  };
  auto field = [&](const char *name, const char *type) {
    const auto *entry =
        program.findField({"Landroid/os/Message;", name, type}, false);
    if (!entry)
      fail(ExecutionStop::Unsupported, "Android message declaration is absent");
    return entry;
  };
  try {
    tick();
    if (!services.uptime_milliseconds)
      fail(ExecutionStop::Unsupported,
           "Android uptime service is not supplied");
    auto time = services.uptime_milliseconds();
    if (time.stop != ExecutionStop::Returned)
      return time;
    if (time.value > INT64_MAX)
      fail(ExecutionStop::Unsupported,
           "Android uptime is outside the modeled range");
    const auto *next = field("next", "Landroid/os/Message;");
    const auto *when = field("when", "J");
    const auto *target = field("target", "Landroid/os/Handler;");
    const auto *flags = field("flags", "I");
    // Retain the same bounded heap links inspected and populated by post.
    // Validate and charge the complete chain before changing any link.
    std::vector<std::shared_ptr<Object>> pending;
    std::set<uint64_t> seen;
    uint64_t current = message_head;
    while (current) {
      tick();
      if (!seen.insert(current).second)
        fail(ExecutionStop::Invalid, "Android Message queue contains a cycle");
      if (pending.size() >= ::neverd::limits::dalvik::kMaxQueuedMessages)
        fail(ExecutionStop::HeapLimit, "Android Message queue limit exhausted");
      auto item = heap.get(current);
      if (!item || !item->Initialized || item->Type != Object::Instance ||
          item->ClassName != "android/os/Message")
        fail(ExecutionStop::Invalid,
             "Android Message queue contains an invalid link");
      for (const auto *entry : {next, when, target, flags})
        if (!item->Fields.contains(entry))
          fail(ExecutionStop::Unsupported,
               "Android message field storage is unknown");
      if (const auto id = item->Fields.at(target)) {
        auto handler = heap.get(id);
        if (!handler || !handler->Initialized ||
            !program.assignable(objectDescriptor(*handler),
                                "Landroid/os/Handler;"))
          fail(ExecutionStop::Invalid,
               "Android Message queue has an invalid target");
      }
      if (item->Fields.at(flags) > UINT32_MAX)
        fail(ExecutionStop::Invalid, "Android Message flags are not an int");
      current = item->Fields.at(next);
      pending.push_back(std::move(item));
    }
    size_t selected = 0;
    if (!pending.empty() && !pending.front()->Fields.at(target)) {
      // A synchronization barrier blocks synchronous messages, including
      // later targets. Only the first following asynchronous message wins.
      selected = 1;
      while (selected < pending.size() &&
             !(pending[selected]->Fields.at(flags) & 2)) {
        tick();
        ++selected;
      }
    }
    if (selected == pending.size())
      fail(ExecutionStop::Unsupported,
           "Android MessageQueue blocking wait is unmodeled");
    auto &message = pending[selected];
    if (std::bit_cast<int64_t>(message->Fields.at(when)) > int64_t(time.value))
      fail(ExecutionStop::Unsupported,
           "Android MessageQueue delayed wait is unmodeled");
    const auto following = message->Fields.at(next);
    tick();
    if (selected)
      pending[selected - 1]->Fields.at(next) = following;
    else
      message_head = following;
    message->Fields.at(next) = 0;
    message->Fields.at(flags) |= 1;
    main_queue_blocked = false;
    return {ExecutionStop::Returned, message->Identity};
  } catch (Stop &stopped) {
    return std::move(stopped.result);
  }
}

std::optional<AndroidApplicationSnapshot::ClassLookupFailure::Loader>
AndroidFramework::definingLoader(std::string_view type,
                                 const Program &program) const {
  using Loader = AndroidApplicationSnapshot::ClassLookupFailure::Loader;
  if (!snapshot)
    return std::nullopt;
  const bool array = type.starts_with('[');
  while (type.starts_with('['))
    type.remove_prefix(1);
  if (array && type.size() == 1 &&
      std::string_view("ZBCSIJFD").find(type[0]) != std::string_view::npos)
    return Loader::Boot;
  if (!program.knownReference(type))
    return std::nullopt;
  if (platformType(type))
    return Loader::Boot;
  if (androidApplicationLoaderState(*snapshot) == true)
    return Loader::Application;
  return std::nullopt;
}

ExecutionResult AndroidFramework::execute(Heap &heap, const Program &program,
                                          const LibraryMethod &method,
                                          uint64_t receiver,
                                          std::span<const uint64_t> arguments,
                                          const AdmitInstruction &admit) {
  if (method.Op == ThreadInstrumentation)
    return instrumentationValue(heap, receiver, {}, admit);
  if (method.Op == HandlerPost)
    return post(heap, program, receiver, arguments[0], admit);
  if (method.Op == MessageQueueNext)
    return nextMessage(heap, program, receiver, admit);
  struct Stop {
    ExecutionResult result;
  };
  auto stop = [](ExecutionStop reason, const char *text) -> uint64_t {
    throw Stop{{reason, 0, {}, text}};
  };
  auto unsupported = [&](const char *text) {
    return stop(ExecutionStop::Unsupported, text);
  };
  auto tick = [&] {
    auto result = admit();
    if (result != ExecutionStop::Returned)
      stop(result, "Android framework work budget exhausted");
  };
  auto ref = [&](const std::shared_ptr<Object> &object) -> uint32_t {
    if (!object)
      stop(ExecutionStop::HeapLimit,
           "Android framework storage budget exhausted");
    return object->Identity;
  };
  auto identity = [&](uint32_t &slot, const char *name,
                      uint64_t extra = 0) -> uint32_t {
    if (!slot) {
      tick();
      slot = ref(heap.allocate(Object::Instance, name, 0, extra));
    }
    return slot;
  };
  auto boot = [&] {
    return identity(boot_loader, "java/lang/BootClassLoader");
  };
  auto appLoader = [&] {
    if (androidApplicationLoaderState(*snapshot) != true)
      unsupported("Android application class loader is not established");
    return identity(app_loader, "dalvik/system/PathClassLoader");
  };
  auto app = [&]() -> uint32_t {
    if (snapshot->phase != AndroidApplicationSnapshot::Phase::Application)
      return 0;
    return identity(application, "android/app/Application");
  };
  auto string = [&](std::string_view text) -> uint32_t {
    auto value = stringValue(heap, text, admit);
    if (value.stop != ExecutionStop::Returned)
      throw Stop{std::move(value)};
    return value.value;
  };
  auto execute = [&]() -> uint64_t {
    if (method.Op == LooperQueue || method.Op == MessageCallback ||
        method.Op == MessageTarget) {
      const bool looper = method.Op == LooperQueue;
      const bool callback = method.Op == MessageCallback;
      const auto *entry = program.findField(
          {looper ? "Landroid/os/Looper;" : "Landroid/os/Message;",
           looper     ? "mQueue"
           : callback ? "callback"
                      : "target",
           looper     ? "Landroid/os/MessageQueue;"
           : callback ? "Ljava/lang/Runnable;"
                      : "Landroid/os/Handler;"},
          false);
      auto object = heap.get(receiver);
      if (!entry || !object || !object->Fields.contains(entry))
        return unsupported("Android framework reference field is not supplied");
      const auto id = object->Fields.at(entry);
      if (id) {
        auto value = heap.get(id);
        if (!value || !value->Initialized ||
            !program.assignable(objectDescriptor(*value),
                                entry->reference.type))
          return stop(ExecutionStop::Invalid,
                      "Android framework reference field has an invalid value");
      }
      tick();
      return id;
    }
    // These ordinary instance operations use real retained field storage.
    // In particular, JNI AllocObject has known zero fields even without an
    // application snapshot; it is not a fabricated prepared main Looper.
    if (method.Op == Construct) {
      auto field = [&](const char *owner, const char *name, const char *type) {
        const auto *result = program.findField(
            {"L" + std::string(owner) + ";", name, type}, false);
        if (!result)
          unsupported("Android Handler declaration is not supplied");
        return result;
      };
      const auto *looper_field =
          field("android/os/Handler", "mLooper", "Landroid/os/Looper;");
      const auto *queue_field =
          field("android/os/Handler", "mQueue", "Landroid/os/MessageQueue;");
      const auto *callback_field = field("android/os/Handler", "mCallback",
                                         "Landroid/os/Handler$Callback;");
      const auto *async_field =
          field("android/os/Handler", "mAsynchronous", "Z");
      auto handler = heap.get(receiver);
      if (handler && (handler->Type != Object::Instance ||
                      !program.assignable(objectDescriptor(*handler),
                                          "Landroid/os/Handler;") ||
                      !handler->Fields.contains(looper_field) ||
                      !handler->Fields.contains(queue_field) ||
                      !handler->Fields.contains(callback_field) ||
                      !handler->Fields.contains(async_field)))
        return stop(ExecutionStop::Invalid,
                    "Android Handler receiver has invalid field storage");
      if (handler && handler->Initialized)
        return stop(ExecutionStop::Invalid,
                    "Android Handler receiver is already initialized");
      if (!arguments[0])
        throw Stop{{ExecutionStop::Thrown, 0, "java/lang/NullPointerException",
                    "Handler requires a nonnull Looper"}};
      const auto *source_queue =
          field("android/os/Looper", "mQueue", "Landroid/os/MessageQueue;");
      auto looper = heap.get(arguments[0]);
      if (!looper->Fields.contains(source_queue))
        return unsupported("Android Looper queue field is not supplied");
      const auto queue = looper->Fields.at(source_queue);
      if (queue) {
        auto object = heap.get(queue);
        if (!object || !object->Initialized ||
            objectDescriptor(*object) != "Landroid/os/MessageQueue;")
          return stop(ExecutionStop::Invalid,
                      "Android Looper queue field has invalid identity");
      }
      tick();
      if (!handler) {
        const auto &fields = program.findClass("Landroid/os/Handler;")->fields;
        handler = heap.allocate(
            Object::Instance, "android/os/Handler", 0,
            fields.size() *
                ::neverd::limits::dalvik::kInstanceFieldStorageBytes);
        ref(handler);
        for (const auto &entry : fields)
          handler->Fields.emplace(&entry, 0);
      }
      handler->Fields.at(looper_field) = arguments[0];
      handler->Fields.at(queue_field) = queue;
      handler->Fields.at(callback_field) = 0;
      handler->Fields.at(async_field) = 0;
      return handler->Identity;
    }
    if ((method.Op == SecureSettingString ||
         method.Op == SecureSettingIntDefault) &&
        !arguments[0])
      throw Stop{{ExecutionStop::Thrown, 0, "java/lang/NullPointerException",
                  "Settings.Secure read requires a resolver"}};
    if (!snapshot || !program.containsClass("Landroid/app/ActivityThread;"))
      return unsupported(
          "Android application lifecycle snapshot is not supplied");
    using Phase = AndroidApplicationSnapshot::Phase;
    switch (method.Op) {
    case MainLooper: {
      if (!snapshot->main_looper_prepared)
        return unsupported("Android main Looper state is not supplied");
      if (!*snapshot->main_looper_prepared)
        return 0;
      if (!main_looper) {
        tick();
        const auto queue =
            ref(heap.allocate(Object::Instance, "android/os/MessageQueue", 0));
        const auto *field = program.findField(
            {"Landroid/os/Looper;", "mQueue", "Landroid/os/MessageQueue;"},
            false);
        auto looper =
            heap.allocate(Object::Instance, "android/os/Looper", 0,
                          ::neverd::limits::dalvik::kInstanceFieldStorageBytes);
        ref(looper);
        looper->Fields.emplace(field, queue);
        main_queue = queue;
        main_looper = looper->Identity;
      }
      return main_looper;
    }
    case ClassLoaderString: {
      const std::optional<AndroidApplicationSnapshot::ClassLoaderPaths> *state;
      if (receiver && receiver == app_loader)
        state = &snapshot->application_loader_paths;
      else if (receiver && receiver == system_loader)
        state = &snapshot->system_loader_paths;
      else
        return unsupported("unmodeled Android PathClassLoader instance");
      if (!*state)
        return unsupported(
            receiver == app_loader
                ? "Android application loader paths are not supplied"
                : "Android system loader paths are not supplied");
      const auto &paths = **state;
      std::string result = "dalvik.system.PathClassLoader[DexPathList[[";
      bool first = true;
      for (const auto &path : paths.dex_zip_files) {
        tick();
        if (!first)
          result += ", ";
        first = false;
        result += "zip file \"" + path + "\"";
      }
      result += "],nativeLibraryDirectories=[";
      first = true;
      for (const auto *list : {&paths.native_library_directories,
                               &paths.system_native_library_directories})
        for (const auto &path : *list) {
          tick();
          if (!first)
            result += ", ";
          first = false;
          result += path;
        }
      result += "]]]";
      return string(result);
    }
    case LocaleDefault:
      if (!snapshot->default_locale)
        return unsupported("Android default Locale is not supplied");
      return identity(default_locale, "java/util/Locale");
    case LocaleLanguage:
    case LocaleCountry: {
      if (!default_locale || receiver != default_locale)
        return unsupported("unmodeled Android Locale instance");
      const bool language = method.Op == LocaleLanguage;
      const auto &value = language ? snapshot->default_locale->language
                                   : snapshot->default_locale->country;
      if (!value)
        return unsupported(language ? "Android Locale language is not supplied"
                                    : "Android Locale country is not supplied");
      auto &retained = language ? locale_language : locale_country;
      if (!retained) {
        const auto bytes = javaStringBytes(*value);
        for (size_t i = 0; i < bytes.size(); i += 2)
          tick();
        // API 28 BaseLocale retains interned components. The same heap pool
        // is used by DEX literals and String.intern reached through JNI.
        retained = ref(heap.literal(*value));
      }
      return retained;
    }
    case SecureSettingString:
    case SecureSettingIntDefault: {
      if (!content_resolver || arguments[0] != content_resolver)
        return unsupported("unmodeled Settings.Secure ContentResolver");
      auto key = heap.get(arguments[1]);
      if (!key)
        return unsupported("null Settings.Secure key requires provider lookup");
      for (size_t i = 0; i < key->Bytes.size(); i += 2)
        tick();
      const std::optional<std::string> *setting;
      uint32_t *retained;
      if (key->Bytes == javaStringBytes("android_id")) {
        if (!snapshot->secure_settings_cache)
          return unsupported("Android secure settings cache is not supplied");
        setting = &snapshot->secure_settings_cache->android_id;
        retained = &secure_android_id;
      } else if (key->Bytes == javaStringBytes("adb_enabled")) {
        // API 28 moves this deprecated Secure key to the Global cache.
        if (!snapshot->global_settings_cache)
          return unsupported("Android global settings cache is not supplied");
        setting = &snapshot->global_settings_cache->adb_enabled;
        retained = &global_adb_enabled;
      } else {
        return unsupported("unmodeled Settings.Secure key");
      }
      const auto &value = *setting;
      if (value && !*retained)
        *retained = string(*value);
      if (method.Op == SecureSettingIntDefault) {
        const auto fallback = uint32_t(arguments[2]);
        if (!value)
          return fallback;
        // The admitted subset is ASCII Integer.parseInt in radix ten.
        // Unicode Character.digit tables require a separate versioned model.
        for (unsigned char byte : *value) {
          tick();
          if (byte > 127)
            return unsupported("non-ASCII Settings integer value");
        }
        std::string_view text = *value;
        bool negative = text.starts_with('-');
        if (negative || text.starts_with('+'))
          text.remove_prefix(1);
        if (text.empty())
          return fallback;
        const uint32_t limit = negative ? 2147483648u : 2147483647u;
        uint32_t magnitude = 0;
        for (unsigned char byte : text) {
          if (byte < '0' || byte > '9')
            return fallback;
          const uint32_t digit = byte - '0';
          if (magnitude > (limit - digit) / 10)
            return fallback;
          magnitude = magnitude * 10 + digit;
        }
        return negative ? uint32_t(0u - magnitude) : magnitude;
      }
      if (!value)
        return 0;
      return *retained;
    }
    case CurrentActivityThread:
      return snapshot->phase == Phase::Unattached
                 ? 0
                 : identity(thread, "android/app/ActivityThread");
    case CurrentApplication:
      return app();
    case ThreadApplication:
      if (receiver != thread || !thread)
        return unsupported("unmodeled Android ActivityThread instance");
      return app();
    case ApplicationContext:
    case BaseContext:
    case ContextClassLoader:
    case PackageName:
    case FilesDirectory:
    case ContextContentResolver:
    case ContextApplicationInfo:
    case ContextPackageManager:
    case PackageResourcePath:
      if (!receiver || (receiver != application && receiver != context))
        return unsupported("unmodeled Android Context instance");
      if (method.Op == ApplicationContext)
        return app();
      if (method.Op == BaseContext)
        return identity(context, "android/app/ContextImpl");
      if (method.Op == ContextClassLoader)
        return appLoader();
      if (method.Op == PackageResourcePath) {
        if (!snapshot->package_resource_path)
          return unsupported("Android package resource path is not supplied");
        const auto &path = *snapshot->package_resource_path;
        if (!path)
          return 0;
        // ContextWrapper delegates to the ContextImpl's LoadedApk. mResDir
        // retains its value when the public ApplicationInfo fields are written.
        if (!package_resource_path)
          package_resource_path = string(*path);
        return package_resource_path;
      }
      if (method.Op == ContextApplicationInfo) {
        if (!application_info) {
          auto result = createApplicationInfo(
              heap, ApplicationInfoSource::Context, 0, admit);
          if (result.stop != ExecutionStop::Returned)
            throw Stop{std::move(result)};
          application_info = result.value;
        }
        return application_info;
      }
      if (method.Op == ContextPackageManager) {
        if (!snapshot->package_manager_available)
          return unsupported(
              "Android PackageManager availability is not supplied");
        if (!*snapshot->package_manager_available)
          return 0;
        // ContextWrapper delegates to the ContextImpl's cached manager. The
        // snapshot supplies availability; no service lookup or RPC is inferred.
        return identity(package_manager,
                        "android/app/ApplicationPackageManager");
      }
      if (method.Op == ContextContentResolver)
        // The attached ContextImpl owns one resolver. ContextWrapper delegates
        // to it. Materializing its identity does not acquire a provider or
        // supply a response to any content operation.
        return identity(content_resolver,
                        "android/app/ContextImpl$ApplicationContentResolver");
      if (method.Op == FilesDirectory) {
        if (!snapshot->files_directory)
          return unsupported("Android files directory is not supplied");
        // Existing-directory projection: no directory creation or permission
        // effects are fabricated from a package name or path spelling.
        if (!files_path)
          files_path = string(*snapshot->files_directory);
        return identity(files_directory, "java/io/File");
      }
      if (!snapshot->package_name)
        return unsupported("Android application package name is not supplied");
      if (!package)
        package = string(*snapshot->package_name);
      return package;
    case PackageApplicationInfo:
    case PackageInfoQuery: {
      if (!package_manager || receiver != package_manager)
        return unsupported("unmodeled Android PackageManager instance");
      auto name = heap.get(arguments[0]);
      if (!name)
        return unsupported(
            method.Op == PackageApplicationInfo
                ? "null package name has no supplied application info query"
                : "null package name has no supplied package info query");
      if (name->Length > ::neverd::limits::dalvik::kMaxAndroidClassNameBytes)
        return unsupported("Android package name exceeds the query budget");
      for (size_t i = 0; i < name->Bytes.size(); i += 2)
        tick();
      auto find = [&](const auto &queries) -> std::optional<size_t> {
        for (size_t i = 0; i < queries.size(); ++i) {
          tick();
          const auto &query = queries[i];
          if (uint32_t(query.flags) == uint32_t(arguments[1]) &&
              javaStringBytes(query.package_name) == name->Bytes)
            return i;
        }
        return {};
      };
      const auto index = method.Op == PackageApplicationInfo
                             ? find(snapshot->application_info_queries)
                             : find(snapshot->package_info_queries);
      if (index) {
        if (method.Op == PackageInfoQuery) {
          tick();
          auto result = ref(heap.allocate(
              Object::Instance, "android/content/pm/PackageInfo", 0,
              sizeof(PackageInfoState) +
                  ::neverd::limits::dalvik::kFrameworkFieldStateOverheadBytes));
          PackageInfoState state;
          state.query = *index;
          package_info_states.emplace(result, std::move(state));
          return result;
        }
        auto result = createApplicationInfo(
            heap, ApplicationInfoSource::ApplicationQuery, *index, admit);
        if (result.stop != ExecutionStop::Returned)
          throw Stop{std::move(result)};
        return result.value;
      }
      return unsupported(method.Op == PackageApplicationInfo
                             ? "Android application info query is not supplied"
                             : "Android package info query is not supplied");
    }
    case PackageApplicationLabel: {
      if (!package_manager || receiver != package_manager)
        return unsupported("unmodeled Android PackageManager instance");
      if (!arguments[0])
        throw Stop{{ExecutionStop::Thrown, 0, "java/lang/NullPointerException",
                    "null Android ApplicationInfo"}};
      if (!application_info_states.contains(arguments[0]))
        return unsupported("unmodeled Android ApplicationInfo instance");
      if (!snapshot->package_item_force_safe_labels)
        return unsupported("Android force-safe-labels state is not supplied");
      if (*snapshot->package_item_force_safe_labels)
        return unsupported("Android safe application labels are not modeled");
      auto label = applicationInfoFieldValue(
          heap, arguments[0], ApplicationInfoField::NonLocalizedLabel, {},
          admit);
      if (label.stop != ExecutionStop::Returned)
        throw Stop{std::move(label)};
      if (!label.value)
        return unsupported(
            "Android application label fallback is not supplied");
      // API 28 returns this CharSequence itself, without String conversion,
      // trimming or copying. Public field writes share the same object slot.
      return label.value;
    }
    case FilePath:
    case FileName:
    case FileParent:
    case FileIsAbsolute: {
      if (!receiver || receiver != files_directory)
        return unsupported("unmodeled Java File instance");
      if (method.Op == FilePath)
        return files_path;
      if (method.Op == FileIsAbsolute)
        return 1;
      const auto &path = *snapshot->files_directory;
      const auto slash = path.rfind('/');
      if (method.Op == FileName)
        return string(std::string_view(path).substr(slash + 1));
      if (path == "/")
        return 0;
      return string(std::string_view(path).substr(0, slash ? slash : 1));
    }
    case SystemClassLoader:
      return identity(system_loader, "dalvik/system/PathClassLoader");
    case LoaderParent:
      if (receiver == boot_loader && boot_loader)
        return 0;
      if ((receiver == system_loader && system_loader) ||
          (receiver == app_loader && app_loader))
        return boot();
      return unsupported("unmodeled Android ClassLoader instance");
    case ClassLoaderOf: {
      auto type = heap.get(receiver)->ClassName;
      if (!type.starts_with('['))
        type = "L" + type + ";";
      const auto owner = definingLoader(type, program);
      if (owner == AndroidApplicationSnapshot::ClassLookupFailure::Loader::Boot)
        return boot();
      if (owner)
        return appLoader();
      while (type.starts_with('['))
        type.erase(type.begin());
      if (program.knownReference(type) && !platformType(type))
        return unsupported(
            "Android application class loader is not established");
      return unsupported("unmodeled class loader ownership");
    }
    case LoadClass: {
      if (receiver != boot_loader && receiver != system_loader &&
          receiver != app_loader)
        return unsupported("unmodeled Android ClassLoader instance");
      auto name = heap.get(arguments[0]);
      if (!name)
        throw Stop{{ExecutionStop::Thrown, 0, "java/lang/NullPointerException",
                    "null Android class name"}};
      if (name->Length > ::neverd::limits::dalvik::kMaxAndroidClassNameBytes)
        return unsupported("Android class name exceeds the lookup budget");
      std::string text;
      for (size_t i = 0; i < name->Bytes.size(); i += 2) {
        tick();
        if (name->Bytes[i + 1] || name->Bytes[i] >= 128)
          return unsupported("non-ASCII Android class lookup is not modeled");
        text.push_back(name->Bytes[i] == '.' ? '/' : name->Bytes[i]);
      }
      // A negative outcome requires an exact declaration for this loader.
      // Absence from supplied DEX alone never implies class absence.
      if (text.empty() || text.front() == '[' ||
          std::find(name->Bytes.begin(), name->Bytes.end(), '/') !=
              name->Bytes.end())
        return unsupported("Android class lookup requires a known binary name");
      const auto type = "L" + text + ";";
      using Loader = AndroidApplicationSnapshot::ClassLookupFailure::Loader;
      const auto loader = receiver == boot_loader     ? Loader::Boot
                          : receiver == system_loader ? Loader::System
                                                      : Loader::Application;
      std::string binary = text;
      std::replace(binary.begin(), binary.end(), '/', '.');
      for (const auto &failure : snapshot->class_lookup_failures) {
        if (failure.loader != loader || classLookupName(failure) != binary)
          continue;
        uint32_t message = failure.message ? string(*failure.message) : 0;
        tick();
        auto exception = heap.allocate(Object::Throwable,
                                       "java/lang/ClassNotFoundException", 0);
        auto id = ref(exception);
        exception->Message = message;
        throw Stop{{ExecutionStop::Thrown, 0,
                    "java/lang/ClassNotFoundException",
                    "explicit Android class lookup failure", id}};
      }
      if (!program.knownReference(type) ||
          (!platformType(type) && receiver != app_loader))
        return unsupported(
            "Android class lookup has no supplied declaration in this loader");
      return ref(heap.classObject(text));
    }
    default:
      return stop(ExecutionStop::Invalid,
                  "invalid Android framework operation");
    }
  };
  try {
    return {ExecutionStop::Returned, execute(), {}, {}};
  } catch (Stop &halt) {
    return std::move(halt.result);
  }
}
} // namespace neverd::mobile::dalvik
