//===- Main.cpp - Native offline web MCP executable -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Launch configuration and binary stdio; no analysis subprocess or runtime.
///
//===----------------------------------------------------------------------===//

#include "Stdio.h"
#include "WebTools.h"

#include <iostream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace {
int run(const std::vector<std::string> &Arguments) {
  std::vector<std::string> Inputs;
  for (std::size_t I = 1; I < Arguments.size(); ++I) {
    const auto &Argument = Arguments[I];
    if (Argument == "--stdio")
      continue;
    if (Argument == "--help") {
      std::cerr << "Usage: neverd-web-mcp [--stdio] [--input PATH]...\n"
                   "At most eight inputs. Capture occurs only through a tool "
                   "request.\n";
      return 0;
    }
    if (Argument == "--version") {
      std::cerr << "neverd-web-mcp profile 1 (MCP 2025-06-18)\n";
      return 0;
    }
    if (Argument != "--input" || I + 1 == Arguments.size() ||
        Inputs.size() == neverd::mcp::MaxInputs) {
      std::cerr << "Invalid launch arguments; use --help.\n";
      return 2;
    }
    Inputs.push_back(Arguments[++I]);
  }
  neverd::mcp::WebTools Tools(std::move(Inputs));
  neverd::mcp::Server Service(Tools);
  return neverd::mcp::runStdio(std::cin, std::cout, Service);
}
} // namespace

#ifdef _WIN32
int wmain(int Count, wchar_t **Arguments) {
  if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
      _setmode(_fileno(stdout), _O_BINARY) == -1)
    return 1;
  try {
    std::vector<std::string> UTF8;
    for (int I = 0; I < Count; ++I) {
      const auto Size =
          WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Arguments[I], -1,
                              nullptr, 0, nullptr, nullptr);
      if (Size <= 0 || Size > 32769)
        throw std::invalid_argument("Invalid launch argument");
      std::string Text(Size, '\0');
      if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Arguments[I], -1,
                              Text.data(), Size, nullptr, nullptr) != Size)
        throw std::invalid_argument("Invalid launch argument");
      Text.pop_back();
      UTF8.push_back(std::move(Text));
    }
    return run(UTF8);
#else
int main(int Count, char **Arguments) {
  std::signal(SIGPIPE, SIG_IGN);
  try {
    return run({Arguments, Arguments + Count});
#endif
  } catch (...) {
    std::cerr << "MCP server could not continue.\n";
    return 1;
  }
}
