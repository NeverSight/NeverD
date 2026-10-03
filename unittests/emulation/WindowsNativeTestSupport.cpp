//===- WindowsNativeTestSupport.cpp - Native thread oracle support -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsNativeTestSupport.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Program.h"

#if defined(_WIN32) && defined(_M_X64)
#include <windows.h>
#endif

namespace neverd::emulation::native_test {
#define NEVERD_NATIVE_TEST_VALUE(Name, Value) constexpr uint32_t Name = Value;
#define NEVERD_NATIVE_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "WindowsNativeTestSupport.def"
#undef NEVERD_NATIVE_TEST_TEXT
#undef NEVERD_NATIVE_TEST_VALUE
#if defined(_WIN32) && defined(_M_X64)
// Retain the initial thread to observe its return independently of the
// process's remaining threads. The child uses only original test fixtures.
llvm::Expected<uint32_t> observeNativeThread(
    const std::filesystem::path &Program, llvm::StringRef Argument,
    const std::filesystem::path &Output, const std::filesystem::path &Error,
    uint32_t TimeoutSeconds) {
  struct Handle {
    HANDLE Value;
    ~Handle() {
      if (Value != INVALID_HANDLE_VALUE)
        CloseHandle(Value);
    }
  };
  auto Failure = []() {
    return llvm::errorCodeToError(
        std::error_code(GetLastError(), std::system_category()));
  };
  SECURITY_ATTRIBUTES Attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  const std::filesystem::path Null(NullDevice);
  Handle Input{CreateFileW(Null.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           &Attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr)};
  if (Input.Value == INVALID_HANDLE_VALUE)
    return Failure();
  Handle Out{CreateFileW(Output.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                         &Attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr)};
  if (Out.Value == INVALID_HANDLE_VALUE)
    return Failure();
  Handle Err{CreateFileW(Error.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                         &Attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr)};
  if (Err.Value == INVALID_HANDLE_VALUE)
    return Failure();
  const std::string Name = Program.string();
  auto Command = llvm::sys::flattenWindowsCommandLine({Name, Argument});
  if (!Command)
    return llvm::errorCodeToError(Command.getError());
  STARTUPINFOW Startup{};
  Startup.cb = sizeof(Startup);
  Startup.dwFlags = STARTF_USESTDHANDLES;
  Startup.hStdInput = Input.Value;
  Startup.hStdOutput = Out.Value;
  Startup.hStdError = Err.Value;
  PROCESS_INFORMATION Child{};
  if (!CreateProcessW(Program.c_str(), Command->data(), nullptr, nullptr, TRUE,
                      0, nullptr, nullptr, &Startup, &Child))
    return Failure();
  auto Cleanup = llvm::scope_exit([&] {
    if (WaitForSingleObject(Child.hProcess, 0) == WAIT_TIMEOUT) {
      TerminateProcess(Child.hProcess, NativeCleanupStatus);
      WaitForSingleObject(Child.hProcess,
                          TimeoutSeconds * MillisecondsPerSecond);
    }
    CloseHandle(Child.hThread);
    CloseHandle(Child.hProcess);
  });
  const DWORD Wait = WaitForSingleObject(
      Child.hThread, TimeoutSeconds * MillisecondsPerSecond);
  if (Wait == WAIT_TIMEOUT)
    return llvm::createStringError(std::errc::timed_out, NativeThreadTimeout);
  if (Wait != WAIT_OBJECT_0)
    return Failure();
  DWORD Status;
  if (!GetExitCodeThread(Child.hThread, &Status))
    return Failure();
  return Status;
}
#endif
} // namespace neverd::emulation::native_test
