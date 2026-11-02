//===- AndroidVersion.cpp - Explicit Android version field projection ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 scalar Build.VERSION values over a supplied property snapshot.
//===----------------------------------------------------------------------===//
#include "neverd/dalvik/AndroidProperties.h"
#include "neverd/dalvik/Limits.h"
#include "neverd/dalvik/Runtime.h"

namespace neverd::mobile::dalvik {
void Program::addAndroidVersion(
    const std::map<std::string, std::string> &properties) {
  Class version;
  version.name = "Landroid/os/Build$VERSION;";
  version.superclass = "Ljava/lang/Object;";
  version.access = {"public"};
  version.source_id = "android-api28-version";
  auto add = [&](const char *name, const char *key, const char *fallback) {
    auto found = properties.find(key);
    const std::string text = found == properties.end() ? "" : found->second;
    if (text.size() >=
            ::neverd::limits::dalvik::
                kAndroidVersionProgramAddAndroidVersionAddAddTextSizeLimit ||
        text.find('\0') != std::string::npos)
      throw Error("invalid Android version property value");
    Field field;
    field.reference = {version.name, name,
                       fallback ? "Ljava/lang/String;" : "I"};
    field.access = {"public", "static", "final"};
    PlatformValue value;
    if (fallback) {
      value.value = text.empty() ? std::string(fallback) : text;
      value.intern_string = text.empty();
      // Refuse unadmitted encodings rather than guessing ART replacement.
      (void)javaStringBytes(std::get<std::string>(value.value));
    } else {
      value.value = int64_t(androidPropertyInt(text, 0));
    }
    platform_values.emplace(field.reference, std::move(value));
    version.fields.push_back(std::move(field));
  };
  add("INCREMENTAL", "ro.build.version.incremental", "unknown");
  add("RELEASE", "ro.build.version.release", "unknown");
  add("BASE_OS", "ro.build.version.base_os", "");
  add("SECURITY_PATCH", "ro.build.version.security_patch", "");
  add("SDK", "ro.build.version.sdk", "unknown");
  add("SDK_INT", "ro.build.version.sdk", nullptr);
  add("FIRST_SDK_INT", "ro.product.first_api_level", nullptr);
  add("PREVIEW_SDK_INT", "ro.build.version.preview_sdk", nullptr);
  add("CODENAME", "ro.build.version.codename", "unknown");
  add("MIN_SUPPORTED_TARGET_SDK_INT",
      "ro.build.version.min_supported_target_sdk", nullptr);
  classes.emplace(version.name, std::move(version));
}

const Program::PlatformValue *Program::platformValue(const Field &field) const {
  if (findField(field.reference, true) != &field)
    return nullptr;
  auto found = platform_values.find(field.reference);
  return found == platform_values.end() ? nullptr : &found->second;
}
} // namespace neverd::mobile::dalvik
