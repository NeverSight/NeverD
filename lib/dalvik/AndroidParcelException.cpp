//===- AndroidParcelException.cpp - Shared Parcel exception headers -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 exception envelopes reuse scalar/string writes and Java upcalls.
//===----------------------------------------------------------------------===//

#include "AndroidFramework.h"

#include <cstdint>
#include <string>
#include <utility>

namespace neverd::mobile::dalvik {
ExecutionResult
AndroidFramework::parcelException(Heap &heap, const Program &program,
                                  uint64_t receiver, uint64_t exception,
                                  const AdmitInstruction &admit) {
  auto object =
      heap.get(exception); // Library validated the Exception argument.
  const auto type = object ? objectDescriptor(*object) : std::string{};
  if (object && program.assignable(type, "Landroid/os/Parcelable;"))
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android Parcelable exception class-loader and payload semantics "
            "are not modeled"};
  if (object &&
      program.assignable(type, "Landroid/os/ServiceSpecificException;"))
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android ServiceSpecificException payload serialization is not "
            "modeled"};
  int32_t code = 0;
  for (auto [name, value] : {std::pair{"Ljava/lang/SecurityException;", -1},
                             {"Landroid/os/BadParcelableException;", -2},
                             {"Ljava/lang/IllegalArgumentException;", -3},
                             {"Ljava/lang/NullPointerException;", -4},
                             {"Ljava/lang/IllegalStateException;", -5},
                             {"Landroid/os/NetworkOnMainThreadException;", -6},
                             {"Ljava/lang/UnsupportedOperationException;", -7}})
    if (object && program.assignable(type, name)) {
      code = value;
      break;
    }
  const auto *write =
      libraryMethod({"Landroid/os/Parcel;", "writeInt", {"I"}, "V"}, false);
  const uint64_t tag[] = {uint32_t(code)};
  auto written =
      executeLibrary(heap, *write, receiver, tag, admit, false, &program, this);
  if (written.stop != ExecutionStop::Returned)
    return written;
  // No supported StrictMode path creates gathered violations. Clearing its
  // initial null list follows the tag write, including unsupported exceptions.
  if (!code) {
    if (object && program.assignable(type, "Ljava/lang/RuntimeException;"))
      return {ExecutionStop::Thrown, 0, object->ClassName, {}, exception};
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android Parcel checked/null exception requires Throwable-cause "
            "construction"};
  }
  ExecutionResult result{ExecutionStop::Suspended};
  result.external.emplace();
  result.external->kind = ExternalCall::JavaMethod;
  result.external->java_method = MethodRef{
      "Ljava/lang/Throwable;", "getMessage", {}, "Ljava/lang/String;"};
  result.external->arguments = {exception};
  result.external->complete = [this, &heap, &program, admit, receiver,
                               write](ExecutionResult message) {
    if (message.stop != ExecutionStop::Returned)
      return message;
    const auto *string = libraryMethod(
        {"Landroid/os/Parcel;", "writeString", {"Ljava/lang/String;"}, "V"},
        false);
    const uint64_t text[] = {message.value};
    auto written = executeLibrary(heap, *string, receiver, text, admit, false,
                                  &program, this);
    if (written.stop != ExecutionStop::Returned)
      return written;
    // API 28 starts with stack-trace parceling disabled. Its setter and
    // private field mutation remain explicit unsupported library boundaries.
    const uint64_t no_trace[] = {0};
    return executeLibrary(heap, *write, receiver, no_trace, admit, false,
                          &program, this);
  };
  return result;
}
} // namespace neverd::mobile::dalvik
