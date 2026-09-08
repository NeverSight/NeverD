// Native process oracle: argv/environment/stdio and inherited descendants.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <spawn.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char **environ;
#endif
#endif
namespace {
namespace fs = std::filesystem;
void sleep(unsigned Milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(Milliseconds));
}
std::string hex(const std::string &S) {
  constexpr char Digits[] = "0123456789abcdef";
  std::string Out;
  for (unsigned char C : S) {
    Out += Digits[C >> 4];
    Out += Digits[C & 15];
  }
  return Out;
}
fs::path path(const std::string &S) {
  return fs::path(
      std::u8string(reinterpret_cast<const char8_t *>(S.data()), S.size()));
}
#ifdef _WIN32
std::wstring wide(const std::string &S) { return path(S).native(); }
std::string utf8(const std::wstring &S) {
  if (S.empty())
    return {};
  int Size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, S.data(),
                                 static_cast<int>(S.size()), nullptr, 0,
                                 nullptr, nullptr);
  if (!Size)
    throw std::runtime_error("cannot encode fixture argument");
  std::string Out(Size, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, S.data(),
                      static_cast<int>(S.size()), Out.data(), Size, nullptr,
                      nullptr);
  return Out;
}
std::wstring quote(const std::wstring &S) {
  std::wstring Out = L"\"";
  size_t Slashes = 0;
  for (auto C : S) {
    if (C == L'\\') {
      ++Slashes;
      continue;
    }
    Out.append(C == L'"' ? 2 * Slashes + 1 : Slashes, L'\\');
    Out += C;
    Slashes = 0;
  }
  Out.append(Slashes * 2, L'\\');
  return Out + L'"';
}
#endif
void descendant(const std::vector<std::string> &Args) {
#ifdef _WIN32
  std::wstring Command;
  for (const auto &A : Args) {
    if (!Command.empty())
      Command += L' ';
    Command += quote(wide(A));
  }
  STARTUPINFOW SI{};
  SI.cb = sizeof(SI);
  SI.dwFlags = STARTF_USESTDHANDLES;
  SI.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  SI.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  SI.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION PI{};
  if (!CreateProcessW(wide(Args[0]).c_str(), Command.data(), nullptr, nullptr,
                      TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &SI, &PI))
    throw std::runtime_error("cannot spawn native fixture descendant");
  CloseHandle(PI.hThread);
  CloseHandle(PI.hProcess);
#else
  std::vector<char *> Argv;
  for (const auto &A : Args)
    Argv.push_back(const_cast<char *>(A.c_str()));
  Argv.push_back(nullptr);
#ifdef __APPLE__
  char **Env = *_NSGetEnviron();
#else
  char **Env = environ;
#endif
  pid_t PID;
  if (posix_spawn(&PID, Args[0].c_str(), nullptr, nullptr, Argv.data(), Env))
    throw std::runtime_error("cannot spawn native fixture descendant");
#endif
}
int fakeJadx(const std::vector<std::string> &Args, const fs::path &Mode) {
  auto grow = [] {
#ifdef _WIN32
    const wchar_t *Temporary = _wgetenv(L"JADX_TMP_DIR");
    if (!Temporary)
      throw std::runtime_error("missing fixture temporary directory");
    fs::path Directory(Temporary);
#else
    const char *Temporary = std::getenv("JADX_TMP_DIR");
    if (!Temporary)
      throw std::runtime_error("missing fixture temporary directory");
    auto Directory = path(Temporary);
#endif
    std::ofstream(Directory / "growth.bin", std::ios::binary)
        << std::string(65536, 'x');
    sleep(400);
  };
  if (Args.at(1) == "--version") {
    if (Mode == path("fake-jadx-version-growth"))
      grow();
    std::cout << "1.5.6\n";
    return 0;
  }
  if (Args.at(1) != "--config")
    return 112;
  auto Output = std::find(Args.begin(), Args.end(), "--output-dir");
  if (Output == Args.end() || ++Output == Args.end())
    return 112;
  auto Sources = path(*Output) / "sources";
  fs::create_directories(Sources);
  std::ofstream(Sources / "Fixture.java", std::ios::binary)
      << "class Fixture {}\n";
  if (Mode == path("fake-jadx-output-growth"))
    grow();
  return 0;
}

int run(const std::vector<std::string> &Args) {
  if (Args.size() < 2)
    return 111;
  auto Name = path(Args[0]).stem();
  if (Name == path("fake-jadx-version-growth") ||
      Name == path("fake-jadx-output-growth") ||
      Name == path("fake-jadx-valid"))
    return fakeJadx(Args, Name);
  const auto &Mode = Args[1];
  if (Mode == "echo") {
    for (size_t I = 2; I < Args.size(); ++I)
      std::cout << hex(Args[I]) << '\n';
  } else if (Mode == "stdio") {
    std::cout << (std::cin.get() == std::char_traits<char>::eof() ? "closed\n"
                                                                  : "open\n");
    std::cout << "stdout\n" << std::flush;
    std::cerr << "stderr\n" << std::flush;
  } else if (Mode == "environment") {
#ifdef _WIN32
    auto Key = wide(Args.at(2));
    DWORD Size = GetEnvironmentVariableW(Key.c_str(), nullptr, 0);
    if (!Size)
      std::cout << (GetLastError() == ERROR_ENVVAR_NOT_FOUND ? "unset\n"
                                                             : "set:\n");
    else {
      std::wstring Value(Size, L'\0');
      DWORD Used = GetEnvironmentVariableW(Key.c_str(), Value.data(), Size);
      Value.resize(Used);
      std::cout << "set:" << hex(utf8(Value)) << '\n';
    }
#else
    const char *Value = std::getenv(Args.at(2).c_str());
    std::cout << (Value ? "set:" + hex(Value) : "unset") << '\n';
#endif
  } else if (Mode == "sleep") {
    sleep(std::stoul(Args.at(2)));
  } else if (Mode == "late") {
    sleep(std::stoul(Args.at(3)));
    std::ofstream(path(Args.at(2)), std::ios::binary) << "late";
  } else if (Mode == "spawn") {
    descendant({Args[0], "late", Args.at(2), Args.at(5)});
    std::cout << "spawned\n" << std::flush;
    sleep(std::stoul(Args.at(4)));
    return std::stoi(Args.at(3));
  } else if (Mode == "fail") {
    std::cout << std::string(std::stoul(Args.at(2)), 'x') << "\nproblem\n"
              << std::flush;
    return 7;
  } else if (Mode == "spam") {
    uint64_t Left = std::stoull(Args.at(2));
    std::string Chunk(8192, 'x');
    while (Left) {
      auto N = std::min<uint64_t>(Left, Chunk.size());
      std::cout.write(Chunk.data(), N);
      Left -= N;
    }
  } else if (Mode == "files") {
    auto Directory = path(Args.at(2));
    unsigned Count = std::stoul(Args.at(3));
    std::string Data(std::stoul(Args.at(4)), 'x');
    for (unsigned I = 0; I < Count; ++I)
      std::ofstream(Directory / ("file" + std::to_string(I)), std::ios::binary)
          << Data;
    sleep(std::stoul(Args.at(5)));
  } else if (Mode == "churn") {
    auto Directory = path(Args.at(2));
    auto End =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (std::chrono::steady_clock::now() < End) {
      for (unsigned I = 0; I < 40; ++I) {
        auto Temporary = Directory / ("temporary" + std::to_string(I));
        std::ofstream(Temporary, std::ios::binary) << "temporary";
        fs::remove(Temporary);
      }
    }
  } else if (Mode == "link") {
    fs::create_symlink(path(Args.at(3)), path(Args.at(2)));
    sleep(200);
  } else
    return 112;
  return 0;
}
} // namespace
#ifdef _WIN32
int wmain(int Count, wchar_t **Arguments) {
  try {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> Args;
    for (int I = 0; I < Count; ++I)
      Args.push_back(utf8(Arguments[I]));
    return run(Args);
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 113;
  }
}
#else
int main(int Count, char **Arguments) {
  try {
    return run(std::vector<std::string>(Arguments, Arguments + Count));
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 113;
  }
}
#endif
