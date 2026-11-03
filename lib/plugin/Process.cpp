//===- Process.cpp - Native provider process transport --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Background pipe I/O with Windows job/POSIX process-group ownership.
///
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Process.h"

#include "neverd/plugin/Config.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace neverd::plugin {
using namespace limits;
namespace {
#ifdef _WIN32
using Pipe = HANDLE;
constexpr Pipe InvalidPipe = nullptr;
void closePipe(Pipe Value) {
  if (Value)
    CloseHandle(Value);
}
int readPipe(Pipe Value, char *Bytes, size_t Size) {
  DWORD Count = 0;
  return ReadFile(Value, Bytes, DWORD(Size), &Count, nullptr) ? int(Count) : -1;
}
int writePipe(Pipe Value, const char *Bytes, size_t Size) {
  DWORD Count = 0;
  return WriteFile(Value, Bytes, DWORD(Size), &Count, nullptr) ? int(Count)
                                                               : -1;
}
std::wstring wide(const std::string &Text) {
  const int Size = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), int(Text.size()), nullptr, 0);
  std::wstring Result(Size, 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(),
                      int(Text.size()), Result.data(), Size);
  return Result;
}
std::wstring quote(const std::wstring &Text) {
  std::wstring Result = L"\"";
  size_t Slashes = 0;
  for (auto C : Text) {
    if (C == L'\\') {
      ++Slashes;
      continue;
    }
    Result.append(C == L'"' ? 2 * Slashes + 1 : Slashes, L'\\');
    Slashes = 0;
    Result += C;
  }
  Result.append(2 * Slashes, L'\\');
  return Result + L'"';
}
#else
using Pipe = int;
constexpr Pipe InvalidPipe = -1;
void closePipe(Pipe Value) {
  if (Value >= 0)
    ::close(Value);
}
int readPipe(Pipe Value, char *Bytes, size_t Size) {
  ssize_t N;
  do {
    N = ::read(Value, Bytes, Size);
  } while (N < 0 && errno == EINTR);
  return int(N);
}
int writePipe(Pipe Value, const char *Bytes, size_t Size) {
  ssize_t N;
  do {
    N = ::write(Value, Bytes, Size);
  } while (N < 0 && errno == EINTR);
  return int(N);
}
/// Close-on-exec from creation where the platform allows it, so a concurrent
/// spawn on another thread cannot inherit the pipe.
bool openPipe(int (&Ends)[2]) {
#ifdef __linux__
  return ::pipe2(Ends, O_CLOEXEC) == 0;
#else
  if (::pipe(Ends) != 0)
    return false;
  fcntl(Ends[0], F_SETFD, FD_CLOEXEC);
  fcntl(Ends[1], F_SETFD, FD_CLOEXEC);
  return true;
#endif
}
#endif
} // namespace

struct Process::Impl {
  PluginHost::Post Post;
  Notify Changed;
  Pipe Input = InvalidPipe, Output = InvalidPipe, Diagnostic = InvalidPipe;
#ifdef _WIN32
  HANDLE Handle = nullptr, Tree = nullptr;
#else
  pid_t Pid = -1;
  /// Becomes readable when the waiter abandons the provider's pipes.
  int Wake[2]{-1, -1};
#endif
  std::thread Reader, Errors, Writer, Waiter;
  std::atomic<bool> Stopping = false;
  std::mutex Mutex;
  std::condition_variable Ready, Drained;
  std::deque<std::string> Writes;
  size_t WriteBytes = 0;
  /// Pipe threads that have not finished, guarded by Mutex.
  int Active = 0;

  Impl(PluginHost::Post Post, Notify Changed)
      : Post(std::move(Post)), Changed(std::move(Changed)) {}
  ~Impl() {
    closePipe(Input);
    closePipe(Output);
    closePipe(Diagnostic);
#ifdef _WIN32
    closePipe(Handle);
    closePipe(Tree);
#else
    closePipe(Wake[0]);
    closePipe(Wake[1]);
#endif
  }
  void emit(ProcessEvent Event) {
    Post([Callback = Changed, Event = std::move(Event)]() mutable {
      Callback(std::move(Event));
    });
  }
  void stop() {
    {
      // Set under the writer's mutex: between its predicate check and its
      // wait, the writer would otherwise miss this wakeup and never exit.
      std::lock_guard Lock(Mutex);
      if (Stopping.exchange(true))
        return;
    }
    Ready.notify_all();
#ifdef _WIN32
    if (Tree)
      TerminateJobObject(Tree, 1);
#else
    if (Pid > 0)
      ::kill(-Pid, SIGKILL);
#endif
  }
  void finished() {
    {
      std::lock_guard Lock(Mutex);
      --Active;
    }
    Drained.notify_all();
  }
#ifndef _WIN32
  /// Waits for \p Events on \p Value; false once the pipes are abandoned.
  bool ready(int Value, short Events) {
    pollfd Fds[]{{Value, Events, 0}, {Wake[0], POLLIN, 0}};
    while (::poll(Fds, 2, -1) < 0)
      if (errno != EINTR)
        return true; // The following read or write reports the failure.
    return !Fds[1].revents;
  }
#endif
  void read(Pipe Value, ProcessEvent::Kind Kind, size_t Limit) {
    std::array<char, kReadChunkBytes> Bytes;
    size_t Total = 0;
    while (true) {
#ifndef _WIN32
      if (!ready(Value, POLLIN))
        break;
#endif
      const auto N = readPipe(Value, Bytes.data(), Bytes.size());
      if (N <= 0)
        break;
      Total += N;
      if (Total > Limit) {
        emit({ProcessEvent::Error, "Provider pipe output exceeded its limit"});
        stop();
        break;
      }
      emit({Kind, std::string(Bytes.data(), N)});
    }
  }
  void writeInput() {
#ifndef _WIN32
    // Broken provider stdin is an operation failure, not a host SIGPIPE.
    sigset_t Set;
    sigemptyset(&Set);
    sigaddset(&Set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &Set, nullptr);
#endif
    while (true) {
      std::unique_lock Lock(Mutex);
      Ready.wait(Lock, [this] { return Stopping || !Writes.empty(); });
      if (Stopping)
        return;
      auto Bytes = std::move(Writes.front());
      Writes.pop_front();
      Lock.unlock();
      size_t Offset = 0;
      while (Offset < Bytes.size()) {
        const auto N =
            writePipe(Input, Bytes.data() + Offset, Bytes.size() - Offset);
#ifndef _WIN32
        // stdin is nonblocking, so abandoning the pipes also ends this wait.
        if (N < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          if (!ready(Input, POLLOUT))
            return;
          continue;
        }
#endif
        if (N <= 0) {
          if (!Stopping)
            emit({ProcessEvent::Error, "Provider stdin closed"});
          return;
        }
        Offset += N;
      }
      Lock.lock();
      WriteBytes -= Bytes.size();
    }
  }
  void waitForExit() {
    int Code = -1;
#ifdef _WIN32
    WaitForSingleObject(Handle, INFINITE);
    DWORD Exit = 0;
    if (GetExitCodeProcess(Handle, &Exit))
      Code = int(Exit);
#else
    int Status = 0;
    pid_t Result;
    do {
      Result = waitpid(Pid, &Status, 0);
    } while (Result < 0 && errno == EINTR);
    if (Result > 0)
      Code = WIFEXITED(Status) ? WEXITSTATUS(Status) : 128 + WTERMSIG(Status);
#endif
    stop();
    bool Abandoned = false;
#ifndef _WIN32
    // stop() signals the provider's process group. A descendant that left the
    // group survives it and can hold the pipes open indefinitely.
    {
      std::unique_lock Lock(Mutex);
      Abandoned =
          !Drained.wait_for(Lock, std::chrono::milliseconds(kShutdownWaitMs),
                            [this] { return Active == 0; });
    }
    if (Abandoned) {
      const char Byte = 0;
      while (::write(Wake[1], &Byte, 1) < 0 && errno == EINTR) {
      }
    }
#endif
    Reader.join();
    Errors.join();
    Writer.join();
    if (Abandoned)
      emit({ProcessEvent::Error,
            "Provider descendants kept its pipes open after it exited"});
    emit({ProcessEvent::Exit, {}, Code});
  }
  void threads() {
    Active = 3;
    Reader = std::thread([this] {
      read(Output, ProcessEvent::Output, kMaxTransportBytes);
      finished();
    });
    Errors = std::thread([this] {
      read(Diagnostic, ProcessEvent::Diagnostic, kMaxLogBytes);
      finished();
    });
    Writer = std::thread([this] {
      writeInput();
      finished();
    });
    Waiter = std::thread([this] { waitForExit(); });
  }
};

Process::Process(PluginHost::Post Post, Notify Changed)
    : State(std::make_unique<Impl>(std::move(Post), std::move(Changed))) {}
Process::~Process() {
  stop();
  join();
}

bool Process::start(const std::filesystem::path &Program,
                    const std::vector<std::string> &Arguments,
                    const std::filesystem::path &Directory,
                    std::string &Error) {
  if (State->Waiter.joinable()) {
    Error = "Provider process already started";
    return false;
  }
#ifdef _WIN32
  SECURITY_ATTRIBUTES Security{sizeof(Security), nullptr, TRUE};
  HANDLE ChildIn = nullptr, ChildOut = nullptr, ChildErr = nullptr;
  const auto Fail = [&](const char *Operation) {
    Error = std::string(Operation) + " (Windows error " +
            std::to_string(GetLastError()) + ")";
    closePipe(ChildIn);
    closePipe(ChildOut);
    closePipe(ChildErr);
    return false;
  };
  if (!CreatePipe(&ChildIn, &State->Input, &Security, 0) ||
      !CreatePipe(&State->Output, &ChildOut, &Security, 0) ||
      !CreatePipe(&State->Diagnostic, &ChildErr, &Security, 0))
    return Fail("CreatePipe failed");
  if (!SetHandleInformation(State->Input, HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(State->Output, HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(State->Diagnostic, HANDLE_FLAG_INHERIT, 0))
    return Fail("Set pipe inheritance failed");
  State->Tree = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
  Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!State->Tree ||
      !SetInformationJobObject(State->Tree, JobObjectExtendedLimitInformation,
                               &Limits, sizeof(Limits)))
    return Fail("Create provider job failed");
  // Restrict inheritance to the three protocol pipes, even when other hosts
  // are starting providers concurrently in this process.
  SIZE_T Bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &Bytes);
  std::vector<char> Storage(Bytes);
  STARTUPINFOEXW Startup{};
  Startup.StartupInfo.cb = sizeof(Startup);
  Startup.lpAttributeList =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(Storage.data());
  if (!InitializeProcThreadAttributeList(Startup.lpAttributeList, 1, 0, &Bytes))
    return Fail("Initialize process attributes failed");
  HANDLE Inherited[]{ChildIn, ChildOut, ChildErr};
  if (!UpdateProcThreadAttribute(Startup.lpAttributeList, 0,
                                 PROC_THREAD_ATTRIBUTE_HANDLE_LIST, Inherited,
                                 sizeof(Inherited), nullptr, nullptr)) {
    DeleteProcThreadAttributeList(Startup.lpAttributeList);
    return Fail("Set process pipe attributes failed");
  }
  Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  Startup.StartupInfo.hStdInput = ChildIn;
  Startup.StartupInfo.hStdOutput = ChildOut;
  Startup.StartupInfo.hStdError = ChildErr;
  std::wstring Command = quote(Program.wstring());
  for (const auto &Argument : Arguments)
    Command += L" " + quote(wide(Argument));
  PROCESS_INFORMATION Info{};
  const bool Created = CreateProcessW(
      Program.c_str(), Command.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
      nullptr, Directory.empty() ? nullptr : Directory.c_str(),
      &Startup.StartupInfo, &Info);
  DeleteProcThreadAttributeList(Startup.lpAttributeList);
  if (!Created)
    return Fail("CreateProcess failed");
  State->Handle = Info.hProcess;
  if (!AssignProcessToJobObject(State->Tree, Info.hProcess)) {
    TerminateProcess(Info.hProcess, 1);
    WaitForSingleObject(Info.hProcess, INFINITE);
    closePipe(Info.hThread);
    return Fail("Assign provider to job failed");
  }
  if (ResumeThread(Info.hThread) == DWORD(-1)) {
    TerminateProcess(Info.hProcess, 1);
    WaitForSingleObject(Info.hProcess, INFINITE);
    closePipe(Info.hThread);
    return Fail("Resume provider failed");
  }
  closePipe(Info.hThread);
  closePipe(ChildIn);
  closePipe(ChildOut);
  closePipe(ChildErr);
#else
  // stdin, stdout, stderr and the waiter's wake pipe.
  int Pipes[4][2]{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}};
  for (auto &P : Pipes) {
    if (!openPipe(P)) {
      Error = std::strerror(errno);
      for (auto &Open : Pipes) {
        closePipe(Open[0]);
        closePipe(Open[1]);
      }
      return false;
    }
  }
  fcntl(Pipes[0][1], F_SETFL, fcntl(Pipes[0][1], F_GETFL) | O_NONBLOCK);
  posix_spawn_file_actions_t Actions;
  posix_spawn_file_actions_init(&Actions);
  posix_spawn_file_actions_adddup2(&Actions, Pipes[0][0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&Actions, Pipes[1][1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&Actions, Pipes[2][1], STDERR_FILENO);
#if defined(__GLIBC__) &&                                                      \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
  // Like the Windows handle list: the provider inherits only the protocol
  // pipes, not descriptors the host opened without close-on-exec.
  posix_spawn_file_actions_addclosefrom_np(&Actions, STDERR_FILENO + 1);
#endif
  if (!Directory.empty())
    posix_spawn_file_actions_addchdir_np(&Actions, Directory.c_str());
  posix_spawnattr_t Attributes;
  posix_spawnattr_init(&Attributes);
  short Flags = POSIX_SPAWN_SETPGROUP;
#ifdef __APPLE__
  Flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  posix_spawnattr_setflags(&Attributes, Flags);
  posix_spawnattr_setpgroup(&Attributes, 0);
  std::vector<std::string> Words{Program.string()};
  Words.insert(Words.end(), Arguments.begin(), Arguments.end());
  std::vector<char *> Argv;
  for (auto &Word : Words)
    Argv.push_back(Word.data());
  Argv.push_back(nullptr);
  const int Result = posix_spawn(&State->Pid, Program.c_str(), &Actions,
                                 &Attributes, Argv.data(), environ);
  posix_spawn_file_actions_destroy(&Actions);
  posix_spawnattr_destroy(&Attributes);
  closePipe(Pipes[0][0]);
  closePipe(Pipes[1][1]);
  closePipe(Pipes[2][1]);
  State->Input = Pipes[0][1];
  State->Output = Pipes[1][0];
  State->Diagnostic = Pipes[2][0];
  State->Wake[0] = Pipes[3][0];
  State->Wake[1] = Pipes[3][1];
  if (Result) {
    Error = std::strerror(Result);
    return false;
  }
#endif
  State->threads();
  Error.clear();
  return true;
}
bool Process::write(std::string Bytes) {
  std::lock_guard Lock(State->Mutex);
  if (State->Stopping || State->WriteBytes + Bytes.size() > kMaxFrameBytes + 4)
    return false;
  State->WriteBytes += Bytes.size();
  State->Writes.push_back(std::move(Bytes));
  State->Ready.notify_one();
  return true;
}
void Process::stop() { State->stop(); }
void Process::join() {
  if (State->Waiter.joinable())
    State->Waiter.join();
}
} // namespace neverd::plugin
