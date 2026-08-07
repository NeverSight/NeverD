#ifndef NEVERD_UNITTESTS_TESTPROCESS_H
#define NEVERD_UNITTESTS_TESTPROCESS_H

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace neverd::test {

inline unsigned long long currentProcessId() {
#ifdef _WIN32
  return static_cast<unsigned long long>(::_getpid());
#else
  return static_cast<unsigned long long>(::getpid());
#endif
}

inline int runShellCommand(std::string_view Command) {
#ifdef _WIN32
  std::string NativeCommand;
  NativeCommand.reserve(Command.size() + 2);
  NativeCommand.push_back('"');
  NativeCommand.append(Command);
  NativeCommand.push_back('"');
#else
  std::string NativeCommand(Command);
#endif
  return std::system(NativeCommand.c_str());
}

inline int systemExitCode(int Status) {
  if (Status == -1)
    return -1;
#ifdef _WIN32
  return Status;
#else
  if (WIFEXITED(Status))
    return WEXITSTATUS(Status);
  if (WIFSIGNALED(Status))
    return 128 + WTERMSIG(Status);
  return -1;
#endif
}

inline std::string shellQuote(std::string_view Value) {
#ifdef _WIN32
  std::string Result = "\"";
  std::size_t Backslashes = 0;
  for (char C : Value) {
    if (C == '\\') {
      ++Backslashes;
      continue;
    }
    if (C == '"') {
      Result.append(Backslashes * 2 + 1, '\\');
      Result.push_back('"');
      Backslashes = 0;
      continue;
    }
    Result.append(Backslashes, '\\');
    Backslashes = 0;
    Result.push_back(C);
  }
  Result.append(Backslashes * 2, '\\');
  Result.push_back('"');
  return Result;
#else
  std::string Result = "'";
  for (char C : Value) {
    if (C == '\'')
      Result += "'\\''";
    else
      Result.push_back(C);
  }
  Result.push_back('\'');
  return Result;
#endif
}

inline const char *nullDevice() {
#ifdef _WIN32
  return "NUL";
#else
  return "/dev/null";
#endif
}

inline const char *executableSuffix() {
#ifdef _WIN32
  return ".exe";
#else
  return "";
#endif
}

inline std::string silenceOutput() {
  return " >" + shellQuote(nullDevice()) + " 2>&1";
}

inline std::string redirectOutput(std::string_view StdoutPath,
                                  std::string_view StderrPath) {
  return " >" + shellQuote(StdoutPath) + " 2>" + shellQuote(StderrPath);
}

inline std::string redirectStdout(std::string_view Path) {
  return " >" + shellQuote(Path);
}

inline std::string silenceStderr() {
  return " 2>" + shellQuote(nullDevice());
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_TESTPROCESS_H
