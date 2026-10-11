//===- Catalog.h - Typed offline MCP tools ----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MCP argument schemas and the supported subset of backend operations.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "Server.h"

#include <vector>

namespace neverd::mcp {
struct Field {
  enum class Kind { String, Decimal, Unsigned, StringArray };
  std::string Name;
  Kind Type = Kind::String;
  bool Required = true;
  std::size_t Maximum = 64;
  std::size_t Minimum = 1;
  Json Choices = Json::array();
};

struct Tool {
  enum class Adapter { Backend, Capabilities, InputPreview };
  std::string Name, Operation, Description;
  std::vector<Field> Fields;
  Adapter Dispatch = Adapter::Backend;
  bool ReadOnly = false;

  Json definition() const;
  /// Validate MCP's declared argument subset before calling the backend.
  Json payload(const Json &Arguments) const;
};

class Catalog {
  std::vector<Tool> Tools;

public:
  Catalog(const Json &BackendCapabilities, std::size_t InputCount);
  const Tool &find(std::string_view Name) const;
  Json list(const Json &Parameters) const;
  Json names() const;
};
} // namespace neverd::mcp
