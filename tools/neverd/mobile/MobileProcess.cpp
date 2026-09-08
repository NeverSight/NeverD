//===- MobileProcess.cpp - Bounded native backend processes
//----------------===//
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include "MobileCommon.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __GLIBC__
#if __GLIBC_PREREQ(2, 34)
#define NEVERD_MOBILE_SPAWN_CLOSEFROM 1
#endif
#endif
#ifdef __APPLE__
#include <crt_externs.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#else
extern char **environ;
#endif
#endif

namespace neverd::mobile {
namespace {
constexpr uint64_t LogLimit = UINT64_C(16) * 1024 * 1024;
using Clock = std::chrono::steady_clock;
using Environment = std::map<std::string, std::optional<std::string>>;

[[noreturn]] void processError(std::string_view What) {
#ifdef _WIN32
  throw Error(std::string(What) + " (Windows error " +
              std::to_string(GetLastError()) + ")");
#else
  throw Error(std::string(What) + ": " + std::strerror(errno));
#endif
}

void validateArguments(const std::vector<std::string> &Args, uint64_t Timeout,
                       const Environment &Overrides) {
  if (Args.empty() || Args[0].empty() || !Timeout ||
      Timeout > uint64_t(std::numeric_limits<int64_t>::max() / 1000000000))
    throw Error("invalid backend command or timeout");
  for (const auto &Arg : Args)
    if (Arg.find('\0') != std::string::npos)
      throw Error("backend argument contains NUL");
  for (const auto &[Name, Value] : Overrides)
    if (Name.empty() || Name.find_first_of("=\0", 0, 2) != std::string::npos ||
        (Value && Value->find('\0') != std::string::npos))
      throw Error("invalid backend environment override");
#ifdef _WIN32
  auto Extension = lowerASCII(pathText(pathFromUTF8(Args[0]).extension()));
  if (Extension == ".bat" || Extension == ".cmd")
    throw Error(
        "batch launchers are unsupported; configure a native executable");
#endif
}

Limits workLimits(const Limits &Input) {
  Input.validate();
  Limits Result = Input;
  auto triple = [](uint64_t N) {
    return N > std::numeric_limits<uint64_t>::max() / 3
               ? std::numeric_limits<uint64_t>::max()
               : N * 3;
  };
  Result.max_files = triple(Result.max_files);
  Result.max_bytes = triple(Result.max_bytes);
  return Result;
}

void checkDeadline(Clock::time_point Deadline, uint64_t Timeout) {
  if (Clock::now() >= Deadline)
    throw Error("backend timed out after " + std::to_string(Timeout) +
                " seconds");
}

bool retired(const std::error_code &EC, bool Live) {
  return Live && EC == std::errc::no_such_file_or_directory;
}

// Live backends may remove a temporary entry while it is being enumerated.
// Only ENOENT is tolerated, and the final scan after process cleanup is strict.
void scanWorkspace(const fs::path &Root, const Limits &L, bool Live,
                   Clock::time_point Deadline, uint64_t Timeout) {
  if (Root.empty())
    return;
  std::error_code EC;
  if (!fs::is_directory(fs::symlink_status(Root, EC)) || EC)
    throw Error("backend workspace root must remain a directory");
  uint64_t Files = 0, Bytes = 0;
  std::vector<fs::path> Pending{Root};
  while (!Pending.empty()) {
    checkDeadline(Deadline, Timeout);
    auto Directory = std::move(Pending.back());
    Pending.pop_back();
    fs::directory_iterator It(Directory, EC), End;
    if (EC) {
      if (retired(EC, Live)) {
        EC.clear();
        continue;
      }
      throw Error("cannot enumerate backend workspace: " + EC.message());
    }
    while (It != End) {
      checkDeadline(Deadline, Timeout);
      auto Path = It->path();
      auto Status = It->symlink_status(EC);
      if (EC || Status.type() == fs::file_type::not_found) {
        if (!retired(EC, Live) &&
            !(Live && Status.type() == fs::file_type::not_found))
          throw Error("cannot inspect backend workspace: " + EC.message());
        EC.clear();
      } else {
        if (!fs::is_regular_file(Status) && !fs::is_directory(Status))
          throw Error("generated output contains a link or special file");
        if (Files == L.max_files)
          throw Error(
              "generated output exceeds the configured file-count limits");
        ++Files;
        if (fs::is_directory(Status))
          Pending.push_back(Path);
        else {
          auto Size = It->file_size(EC);
          if (EC) {
            if (!retired(EC, Live))
              throw Error("cannot size backend workspace entry: " +
                          EC.message());
            EC.clear();
          } else {
            if (Size > L.max_bytes - Bytes)
              throw Error(
                  "generated output exceeds the configured byte limits");
            Bytes += Size;
          }
        }
      }
      It.increment(EC);
      if (EC) {
        if (retired(EC, Live)) {
          EC.clear();
          break;
        }
        throw Error("cannot enumerate backend workspace: " + EC.message());
      }
    }
  }
}

#ifdef _WIN32
class Handle {
  HANDLE H = INVALID_HANDLE_VALUE;

public:
  Handle() = default;
  explicit Handle(HANDLE Value) : H(Value) {}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  ~Handle() { reset(); }
  HANDLE get() const { return H; }
  bool valid() const { return H && H != INVALID_HANDLE_VALUE; }
  void reset(HANDLE Value = INVALID_HANDLE_VALUE) {
    if (valid())
      CloseHandle(H);
    H = Value;
  }
};
#else
class Descriptor {
  int FD = -1;

public:
  Descriptor() = default;
  explicit Descriptor(int Value) : FD(Value) {}
  Descriptor(const Descriptor &) = delete;
  Descriptor &operator=(const Descriptor &) = delete;
  ~Descriptor() { reset(); }
  int get() const { return FD; }
  void reset(int Value = -1) {
    if (FD >= 0)
      ::close(FD);
    FD = Value;
  }
};
#endif

class Log {
#ifdef _WIN32
  Handle File;
#else
  Descriptor File;
#endif
  uint64_t Written = 0;

public:
  explicit Log(const fs::path &Path) {
    if (!Path.parent_path().empty())
      fs::create_directories(Path.parent_path());
#ifdef _WIN32
    File.reset(CreateFileW(Path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr));
    if (!File.valid())
      processError("cannot exclusively create backend log");
#else
    File.reset(::open(Path.c_str(),
                      O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                      0600));
    if (File.get() < 0)
      processError("cannot exclusively create backend log");
#endif
  }
  void append(const char *Data, size_t Size) {
    size_t Keep =
        static_cast<size_t>(std::min<uint64_t>(Size, LogLimit - Written));
    for (size_t Offset = 0; Offset < Keep;) {
#ifdef _WIN32
      DWORD Count = 0;
      if (!WriteFile(File.get(), Data + Offset,
                     static_cast<DWORD>(Keep - Offset), &Count, nullptr) ||
          !Count)
        processError("cannot write backend log");
#else
      ssize_t Count = ::write(File.get(), Data + Offset, Keep - Offset);
      if (Count < 0 && errno == EINTR)
        continue;
      if (Count <= 0)
        processError("cannot write backend log");
#endif
      Offset += static_cast<size_t>(Count);
    }
    Written += Keep;
    if (Keep != Size)
      throw Error("backend diagnostic output exceeded 16 MiB");
  }
  std::string tail() {
    size_t Size = static_cast<size_t>(std::min<uint64_t>(Written, 4096));
    std::string Result(Size, '\0');
#ifdef _WIN32
    LARGE_INTEGER Offset;
    Offset.QuadPart = static_cast<LONGLONG>(Written - Size);
    if (!SetFilePointerEx(File.get(), Offset, nullptr, FILE_BEGIN))
      processError("cannot seek backend log");
    DWORD Count = 0;
    if (!ReadFile(File.get(), Result.data(), static_cast<DWORD>(Size), &Count,
                  nullptr))
      processError("cannot read backend log");
    Result.resize(Count);
#else
    size_t Done = 0;
    while (Done < Size) {
      auto Count = ::pread(File.get(), Result.data() + Done, Size - Done,
                           static_cast<off_t>(Written - Size + Done));
      if (Count < 0 && errno == EINTR)
        continue;
      if (Count < 0)
        processError("cannot read backend log");
      if (!Count)
        break;
      Done += static_cast<size_t>(Count);
    }
    Result.resize(Done);
#endif
    if (!llvm::json::isUTF8(Result))
      Result = llvm::json::fixUTF8(Result);
    while (!Result.empty() && (Result.back() == '\n' || Result.back() == '\r' ||
                               Result.back() == ' ' || Result.back() == '\t'))
      Result.pop_back();
    return Result;
  }
};

#ifndef _WIN32
int privateDescriptor(int FD) {
  if (FD < 0)
    processError("cannot create backend descriptor");
  int Result = ::fcntl(FD, F_DUPFD_CLOEXEC, 3);
  int Saved = errno;
  ::close(FD);
  if (Result < 0) {
    errno = Saved;
    processError("cannot isolate backend descriptor");
  }
  return Result;
}

std::vector<std::string> childEnvironment(const Environment &Overrides) {
  std::map<std::string, std::string> Values;
#ifdef __APPLE__
  char **Current = *_NSGetEnviron();
#else
  char **Current = environ;
#endif
  for (; Current && *Current; ++Current) {
    std::string Entry(*Current);
    auto Split = Entry.find('=');
    if (Split != std::string::npos)
      Values[Entry.substr(0, Split)] = Entry.substr(Split + 1);
  }
  for (const auto &[Name, Value] : Overrides) {
    if (Value)
      Values[Name] = *Value;
    else
      Values.erase(Name);
  }
  std::vector<std::string> Result;
  for (const auto &[Name, Value] : Values)
    Result.push_back(Name + "=" + Value);
  return Result;
}

std::string executablePath(const std::string &Name,
                           const std::vector<std::string> &Env) {
  if (Name.find('/') != std::string::npos)
    return Name;
  std::string Path = "/usr/bin:/bin";
  for (const auto &Entry : Env)
    if (Entry.starts_with("PATH=")) {
      Path = Entry.substr(5);
      break;
    }
  bool Denied = false;
  for (size_t Begin = 0;;) {
    size_t End = Path.find(':', Begin);
    auto Directory =
        Path.substr(Begin, End == std::string::npos ? End : End - Begin);
    auto Candidate = Directory.empty() ? Name : Directory + "/" + Name;
    struct stat S{};
    if (::stat(Candidate.c_str(), &S) == 0 && S_ISREG(S.st_mode)) {
      if (::access(Candidate.c_str(), X_OK) == 0)
        return Candidate;
      if (errno == EACCES)
        Denied = true;
    }
    if (End == std::string::npos)
      break;
    Begin = End + 1;
  }
  errno = Denied ? EACCES : ENOENT;
  processError("cannot execute backend " + Name);
}

#ifdef __APPLE__
bool containsOnlyExitedProcesses(pid_t Leader) {
  // Darwin reports EPERM for killpg when the group contains only zombies.
  // Keep the unreaped leader as the identity anchor: reaping before killpg
  // would permit the kernel to reuse its PID for an unrelated process group.
  int Query[] = {CTL_KERN, KERN_PROC, KERN_PROC_PGRP, Leader};
  size_t Bytes = 0;
  if (::sysctl(Query, 4, nullptr, &Bytes, nullptr, 0) || !Bytes ||
      Bytes > 16 * 1024 * 1024)
    return false;
  std::vector<kinfo_proc> Processes((Bytes + sizeof(kinfo_proc) - 1) /
                                    sizeof(kinfo_proc));
  Bytes = Processes.size() * sizeof(kinfo_proc);
  if (::sysctl(Query, 4, Processes.data(), &Bytes, nullptr, 0) ||
      Bytes % sizeof(kinfo_proc))
    return false;
  bool HasLeader = false;
  for (size_t I = 0; I < Bytes / sizeof(kinfo_proc); ++I) {
    const auto &Process = Processes[I].kp_proc;
    if (Process.p_stat != SZOMB)
      return false;
    HasLeader |= Process.p_pid == Leader;
  }
  return HasLeader;
}
#endif

class Child {
  pid_t PID = -1;
  Descriptor Reader;
  int ExitStatus = 0;
  bool EOFSeen = false;

public:
  Child(const std::vector<std::string> &Args, const Environment &Overrides) {
    auto Env = childEnvironment(Overrides);
    auto Executable = executablePath(Args[0], Env);
    std::vector<char *> ArgPointers, EnvPointers;
    for (const auto &Arg : Args)
      ArgPointers.push_back(const_cast<char *>(Arg.c_str()));
    ArgPointers.push_back(nullptr);
    for (auto &Entry : Env)
      EnvPointers.push_back(Entry.data());
    EnvPointers.push_back(nullptr);
    int Pipe[2];
    if (::pipe(Pipe))
      processError("cannot create backend output pipe");
    Descriptor RawReader(Pipe[0]), RawWriter(Pipe[1]);
    // Dup before releasing the original handles so all spawn sources are >= 3.
    int ReadFD = ::fcntl(RawReader.get(), F_DUPFD_CLOEXEC, 3);
    if (ReadFD < 0)
      processError("cannot isolate backend pipe");
    Reader.reset(ReadFD);
    int WriteFD = ::fcntl(RawWriter.get(), F_DUPFD_CLOEXEC, 3);
    if (WriteFD < 0)
      processError("cannot isolate backend pipe");
    Descriptor Writer(WriteFD);
    RawReader.reset();
    RawWriter.reset();
    Descriptor Input(
        privateDescriptor(::open("/dev/null", O_RDONLY | O_CLOEXEC)));
    int Flags = ::fcntl(Reader.get(), F_GETFL);
    if (Flags < 0 || ::fcntl(Reader.get(), F_SETFL, Flags | O_NONBLOCK))
      processError("cannot configure backend output pipe");
    posix_spawn_file_actions_t Actions;
    posix_spawnattr_t Attributes;
    auto check = [](int Result) {
      if (Result) {
        errno = Result;
        processError("cannot configure backend process");
      }
    };
    check(posix_spawn_file_actions_init(&Actions));
    struct ActionsCleanup {
      posix_spawn_file_actions_t *A;
      ~ActionsCleanup() { posix_spawn_file_actions_destroy(A); }
    } ActionsGuard{&Actions};
    check(posix_spawnattr_init(&Attributes));
    struct AttributesCleanup {
      posix_spawnattr_t *A;
      ~AttributesCleanup() { posix_spawnattr_destroy(A); }
    } AttributesGuard{&Attributes};
    check(
        posix_spawn_file_actions_adddup2(&Actions, Input.get(), STDIN_FILENO));
    check(posix_spawn_file_actions_adddup2(&Actions, Writer.get(),
                                           STDOUT_FILENO));
    check(posix_spawn_file_actions_adddup2(&Actions, Writer.get(),
                                           STDERR_FILENO));
    short SpawnFlags =
        POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    SpawnFlags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#elif defined(NEVERD_MOBILE_SPAWN_CLOSEFROM)
    check(posix_spawn_file_actions_addclosefrom_np(&Actions, 3));
#else
    // Configure closing in the parent, never allocate or enumerate after fork.
    fs::path FDDirectory =
        fs::exists("/proc/self/fd") ? "/proc/self/fd" : "/dev/fd";
    std::vector<int> OpenDescriptors;
    for (const auto &Entry : fs::directory_iterator(FDDirectory)) {
      auto Text = Entry.path().filename().string();
      if (Text.empty() ||
          !std::all_of(Text.begin(), Text.end(),
                       [](unsigned char C) { return C >= '0' && C <= '9'; }))
        continue;
      uint64_t FD = std::stoull(Text);
      if (FD >= 3 &&
          FD <= static_cast<uint64_t>(std::numeric_limits<int>::max()))
        OpenDescriptors.push_back(static_cast<int>(FD));
    }
    // The iterator's own descriptor has closed by this point. Do not add a
    // close action for an invalid descriptor on platforms that reject it.
    for (int FD : OpenDescriptors)
      if (::fcntl(FD, F_GETFD) >= 0)
        check(posix_spawn_file_actions_addclose(&Actions, FD));
#endif
    sigset_t Mask, Defaults;
    sigemptyset(&Mask);
    sigemptyset(&Defaults);
    for (int Signal : {SIGPIPE, SIGINT, SIGQUIT, SIGTERM, SIGHUP})
      sigaddset(&Defaults, Signal);
    check(posix_spawnattr_setpgroup(&Attributes, 0));
    check(posix_spawnattr_setsigmask(&Attributes, &Mask));
    check(posix_spawnattr_setsigdefault(&Attributes, &Defaults));
    check(posix_spawnattr_setflags(&Attributes, SpawnFlags));
    pid_t Started = -1;
    int Result =
        posix_spawn(&Started, Executable.c_str(), &Actions, &Attributes,
                    ArgPointers.data(), EnvPointers.data());
    if (Result) {
      errno = Result;
      processError("cannot execute backend " + Args[0]);
    }
    PID = Started;
  }
  Child(const Child &) = delete;
  ~Child() {
    if (PID > 0) {
      ::kill(-PID, SIGKILL);
      ::kill(PID, SIGKILL);
      int Status;
      while (::waitpid(PID, &Status, 0) < 0 && errno == EINTR) {
      }
    }
  }
  bool exited() {
    siginfo_t Info{};
    int Result;
    do {
      Result = ::waitid(P_PID, static_cast<id_t>(PID), &Info,
                        WEXITED | WNOHANG | WNOWAIT);
    } while (Result < 0 && errno == EINTR);
    if (Result < 0)
      processError("cannot inspect backend process");
    return Info.si_pid == PID;
  }
  void stop() {
    if (PID <= 0)
      return;
    if (::kill(-PID, SIGKILL) && errno != ESRCH) {
      int Saved = errno;
#ifdef __APPLE__
      bool AlreadyExited = Saved == EPERM && containsOnlyExitedProcesses(PID);
#else
      bool AlreadyExited = false;
#endif
      if (!AlreadyExited) {
        errno = Saved;
        processError("cannot stop backend process group");
      }
    }
    int Status = 0;
    pid_t Result;
    do {
      Result = ::waitpid(PID, &Status, 0);
    } while (Result < 0 && errno == EINTR);
    if (Result < 0)
      processError("cannot reap backend process");
    PID = -1;
    ExitStatus = WIFEXITED(Status)     ? WEXITSTATUS(Status)
                 : WIFSIGNALED(Status) ? -WTERMSIG(Status)
                                       : -1;
  }
  bool drain(Log &Output) {
    if (EOFSeen)
      return true;
    std::array<char, 65536> Buffer;
    for (unsigned Reads = 0; Reads < 16; ++Reads) {
      ssize_t Size = ::read(Reader.get(), Buffer.data(), Buffer.size());
      if (Size > 0) {
        Output.append(Buffer.data(), static_cast<size_t>(Size));
        continue;
      }
      if (!Size) {
        EOFSeen = true;
        return true;
      }
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return false;
      processError("cannot read backend output");
    }
    return false; // Yield so a continuous writer cannot starve budget checks.
  }
  int64_t status() const { return ExitStatus; }
  void pause() {
    if (EOFSeen) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      return;
    }
    pollfd FD{Reader.get(), POLLIN, 0};
    if (::poll(&FD, 1, 10) < 0 && errno != EINTR)
      processError("cannot wait for backend output");
  }
};
#else
std::wstring wide(std::string_view Text) {
  if (Text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    throw Error("backend text exceeds Windows API limits");
  if (Text.empty())
    return {};
  int Size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(),
                                 static_cast<int>(Text.size()), nullptr, 0);
  if (!Size)
    processError("backend text must be valid UTF-8");
  std::wstring Result(Size, L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(),
                          static_cast<int>(Text.size()), Result.data(),
                          Size) != Size)
    processError("cannot convert backend text");
  return Result;
}
struct EnvLess {
  bool operator()(const std::wstring &A, const std::wstring &B) const {
    return CompareStringOrdinal(A.data(), static_cast<int>(A.size()), B.data(),
                                static_cast<int>(B.size()),
                                TRUE) == CSTR_LESS_THAN;
  }
};
using WideEnvironment = std::map<std::wstring, std::wstring, EnvLess>;
WideEnvironment childEnvironment(const Environment &Overrides) {
  WideEnvironment Values;
  wchar_t *Raw = GetEnvironmentStringsW();
  if (!Raw)
    processError("cannot copy backend environment");
  struct FreeEnvironment {
    wchar_t *P;
    ~FreeEnvironment() { FreeEnvironmentStringsW(P); }
  } Free{Raw};
  for (const wchar_t *Current = Raw; *Current;) {
    std::wstring Entry(Current);
    Current += Entry.size() + 1;
    // Preserve Windows' hidden per-drive current-directory variables (=C:=...).
    size_t Split = Entry.find(L'=', Entry.starts_with(L'=') ? 1 : 0);
    if (Split != std::wstring::npos)
      Values[Entry.substr(0, Split)] = Entry.substr(Split + 1);
  }
  for (const auto &[Name, Value] : Overrides) {
    auto Key = wide(Name);
    if (Value)
      Values[Key] = wide(*Value);
    else
      Values.erase(Key);
  }
  return Values;
}
std::wstring quote(const std::wstring &Arg) {
  std::wstring Result = L"\"";
  size_t Slashes = 0;
  for (wchar_t C : Arg) {
    if (C == L'\\') {
      ++Slashes;
      continue;
    }
    if (C == L'"') {
      Result.append(Slashes * 2 + 1, L'\\');
      Result += C;
    } else {
      Result.append(Slashes, L'\\');
      Result += C;
    }
    Slashes = 0;
  }
  Result.append(Slashes * 2, L'\\');
  Result += L'"';
  return Result;
}
std::wstring executablePath(const std::string &Name,
                            const WideEnvironment &Env) {
  if (Name.find_first_of("/\\:") != std::string::npos)
    return fs::absolute(pathFromUTF8(Name)).native();
  auto I = Env.find(L"PATH");
  std::wstring Path = I == Env.end() ? L"." : I->second;
  auto Executable = wide(Name);
  DWORD Size = SearchPathW(Path.c_str(), Executable.c_str(), L".exe", 0,
                           nullptr, nullptr);
  if (!Size)
    processError("cannot execute backend " + Name);
  std::wstring Result(Size, L'\0');
  DWORD Used = SearchPathW(Path.c_str(), Executable.c_str(), L".exe", Size,
                           Result.data(), nullptr);
  if (!Used || Used >= Size)
    processError("cannot resolve backend executable");
  Result.resize(Used);
  return Result;
}
class Child {
  Handle Job, Process, Reader;
  int64_t ExitStatus = 0;
  bool Stopped = false, EOFSeen = false;

public:
  Child(const std::vector<std::string> &Args, const Environment &Overrides) {
    auto Env = childEnvironment(Overrides);
    auto Executable = executablePath(Args[0], Env);
    std::wstring Command;
    for (const auto &Arg : Args) {
      if (!Command.empty())
        Command += L' ';
      Command += quote(wide(Arg));
      if (Command.size() >= 32767)
        throw Error("backend command exceeds Windows command-line limits");
    }
    std::vector<wchar_t> Block;
    for (const auto &[Name, Value] : Env) {
      Block.insert(Block.end(), Name.begin(), Name.end());
      Block.push_back(L'=');
      Block.insert(Block.end(), Value.begin(), Value.end());
      Block.push_back(L'\0');
    }
    Block.push_back(L'\0');
    if (Env.empty())
      Block.push_back(L'\0');
    Job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!Job.valid())
      processError("cannot create backend job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Info{};
    Info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(Job.get(), JobObjectExtendedLimitInformation,
                                 &Info, sizeof(Info)))
      processError("cannot configure backend job");
    SECURITY_ATTRIBUTES Security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE Read = INVALID_HANDLE_VALUE, Write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&Read, &Write, &Security, 65536))
      processError("cannot create backend pipe");
    Reader.reset(Read);
    Handle Writer(Write);
    if (!SetHandleInformation(Reader.get(), HANDLE_FLAG_INHERIT, 0))
      processError("cannot isolate backend pipe");
    Handle Input(CreateFileW(L"NUL", GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &Security,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!Input.valid())
      processError("cannot open backend null input");
    SIZE_T AttributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributeBytes);
    if (!AttributeBytes)
      processError("cannot size backend handle attributes");
    std::vector<unsigned char> Attributes(AttributeBytes);
    auto *List =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(Attributes.data());
    if (!InitializeProcThreadAttributeList(List, 1, 0, &AttributeBytes))
      processError("cannot initialize backend handle attributes");
    struct DeleteAttributes {
      LPPROC_THREAD_ATTRIBUTE_LIST P;
      ~DeleteAttributes() { DeleteProcThreadAttributeList(P); }
    } Delete{List};
    HANDLE Inherited[] = {Input.get(), Writer.get()};
    if (!UpdateProcThreadAttribute(List, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   Inherited, sizeof(Inherited), nullptr,
                                   nullptr))
      processError("cannot restrict backend inherited handles");
    STARTUPINFOEXW Startup{};
    Startup.StartupInfo.cb = sizeof(Startup);
    Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    Startup.StartupInfo.hStdInput = Input.get();
    Startup.StartupInfo.hStdOutput = Writer.get();
    Startup.StartupInfo.hStdError = Writer.get();
    Startup.lpAttributeList = List;
    PROCESS_INFORMATION PI{};
    if (!CreateProcessW(Executable.c_str(), Command.data(), nullptr, nullptr,
                        TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                        Block.data(), nullptr, &Startup.StartupInfo, &PI))
      processError("cannot execute backend " + Args[0]);
    Process.reset(PI.hProcess);
    Handle Thread(PI.hThread);
    if (!AssignProcessToJobObject(Job.get(), Process.get())) {
      DWORD Saved = GetLastError();
      TerminateProcess(Process.get(), 1);
      WaitForSingleObject(Process.get(), INFINITE);
      SetLastError(Saved);
      processError("cannot contain backend process in its job");
    }
    if (ResumeThread(Thread.get()) == static_cast<DWORD>(-1)) {
      DWORD Saved = GetLastError();
      TerminateJobObject(Job.get(), 1);
      WaitForSingleObject(Process.get(), INFINITE);
      SetLastError(Saved);
      processError("cannot resume contained backend process");
    }
  }
  Child(const Child &) = delete;
  ~Child() {
    if (!Stopped && Process.valid()) {
      TerminateJobObject(Job.get(), 1);
      WaitForSingleObject(Process.get(), INFINITE);
    }
  }
  bool exited() {
    DWORD Result = WaitForSingleObject(Process.get(), 0);
    if (Result == WAIT_FAILED)
      processError("cannot inspect backend process");
    return Result == WAIT_OBJECT_0;
  }
  void stop() {
    if (Stopped)
      return;
    DWORD Status = 0;
    if (!GetExitCodeProcess(Process.get(), &Status))
      processError("cannot read backend status");
    if (!TerminateJobObject(Job.get(), 1))
      processError("cannot stop backend process job");
    if (WaitForSingleObject(Process.get(), INFINITE) != WAIT_OBJECT_0)
      processError("cannot wait for backend cleanup");
    ExitStatus = Status;
    Stopped = true;
  }
  bool drain(Log &Output) {
    if (EOFSeen)
      return true;
    std::array<char, 65536> Buffer;
    for (unsigned Reads = 0; Reads < 16; ++Reads) {
      DWORD Available = 0;
      if (!PeekNamedPipe(Reader.get(), nullptr, 0, nullptr, &Available,
                         nullptr)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) {
          EOFSeen = true;
          return true;
        }
        processError("cannot inspect backend output pipe");
      }
      if (!Available)
        return false;
      DWORD Count = 0;
      if (!ReadFile(
              Reader.get(), Buffer.data(),
              std::min<DWORD>(Available, static_cast<DWORD>(Buffer.size())),
              &Count, nullptr)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) {
          EOFSeen = true;
          return true;
        }
        processError("cannot read backend output");
      }
      if (!Count) {
        EOFSeen = true;
        return true;
      }
      Output.append(Buffer.data(), Count);
    }
    return false;
  }
  int64_t status() const { return ExitStatus; }
  void pause() {
    if (WaitForSingleObject(Process.get(), 1) == WAIT_FAILED)
      processError("cannot wait for backend output");
  }
};
#endif
} // namespace

void runTool(const std::vector<std::string> &Args, const fs::path &LogPath,
             uint64_t Timeout, const fs::path &Workspace, const Limits &L,
             const Environment &Overrides) {
  validateArguments(Args, Timeout, Overrides);
  Limits Work = workLimits(L);
  auto Now = Clock::now();
  auto Available = std::chrono::duration_cast<std::chrono::seconds>(
      Clock::time_point::max() - Now);
  if (Timeout > static_cast<uint64_t>(Available.count()))
    throw Error("invalid backend timeout: deadline is not representable");
  auto Deadline = Now + std::chrono::seconds(Timeout);
  Log Output(LogPath);
  scanWorkspace(Workspace, Work, false, Deadline, Timeout);
  Child Process(Args, Overrides);
  auto NextScan = Clock::time_point::min();
  for (;;) {
    Process.drain(Output);
    if (Process.exited()) {
      Process
          .stop(); // Descendants must be stopped before inspecting final files.
      auto DrainDeadline =
          std::min(Deadline, Clock::now() + std::chrono::seconds(1));
      while (!Process.drain(Output)) {
        if (Clock::now() >= DrainDeadline)
          throw Error("backend descendants did not close their output pipe");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      scanWorkspace(Workspace, Work, false, Deadline, Timeout);
      break;
    }
    checkDeadline(Deadline, Timeout);
    if (!Workspace.empty() && Clock::now() >= NextScan) {
      scanWorkspace(Workspace, Work, true, Deadline, Timeout);
      NextScan = Clock::now() + std::chrono::milliseconds(250);
    }
    Process.pause();
  }
  if (Process.status() != 0)
    throw Error("backend exited with status " +
                std::to_string(Process.status()) + ": " + Output.tail());
}
} // namespace neverd::mobile
