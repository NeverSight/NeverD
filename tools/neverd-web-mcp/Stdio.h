//===- Stdio.h - MCP newline transport -------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Serial stdio transport independent of the selected tool backend.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <iosfwd>

namespace neverd::mcp {
class Server;
int runStdio(std::istream &Input, std::ostream &Output, Server &Service);
} // namespace neverd::mcp
