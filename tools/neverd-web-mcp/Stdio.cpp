//===- Stdio.cpp - Bounded MCP newline transport --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded input and atomic JSON response construction before stdio writes.
///
//===----------------------------------------------------------------------===//

#include "Stdio.h"

#include "Server.h"

#include <istream>
#include <ostream>

namespace neverd::mcp {
namespace {
bool write(std::ostream &Output, const Json &Message) {
  std::string Bytes;
  try {
    Bytes = transport::serializeJson(Message, MaxResponseBytes);
  } catch (...) {
    Bytes = transport::serializeJson(
        errorReply(Message.value("id", Json(nullptr)), -32603,
                   "Response budget or encoding failure"),
        MaxResponseBytes);
  }
  Output.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
  Output.put('\n');
  Output.flush();
  return static_cast<bool>(Output);
}
} // namespace

int runStdio(std::istream &Input, std::ostream &Output, Server &Service) {
  std::string Line;
  char C;
  while (Input.get(C)) {
    if (C == '\n') {
      if (!Line.empty() && Line.back() == '\r')
        Line.pop_back();
      const auto Reply = Service.handle(Line);
      Line.clear();
      if (Reply && !write(Output, *Reply))
        return 1;
    } else {
      if (Line.size() == MaxRequestBytes) {
        write(Output,
              errorReply(nullptr, -32700, "Request exceeds byte limit"));
        return 2;
      }
      Line.push_back(C);
    }
  }
  if (!Line.empty()) {
    write(Output, errorReply(nullptr, -32700, "Unterminated message"));
    return 2;
  }
  return Input.eof() ? 0 : 1;
}
} // namespace neverd::mcp
