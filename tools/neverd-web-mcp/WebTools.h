//===- WebTools.h - MCP offline web adapter ---------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Process-local configured inputs and dispatch through the shared C API
/// client.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "Catalog.h"
#include "common/web/Backend.h"

namespace neverd::mcp {
inline constexpr std::size_t MaxInputs = 8;

class WebTools final : public ToolService {
  std::vector<std::string> Inputs;
  Json BackendCapabilities;
  Catalog Available;
  web_client::Backend Backend;

  Json capabilities() const;

public:
  /// Paths are private launch configuration, never MCP arguments or output.
  explicit WebTools(std::vector<std::string> Inputs = {});
  Json list(const Json &Parameters) override;
  Json call(const std::string &Name, const Json &Arguments) override;
};
} // namespace neverd::mcp
