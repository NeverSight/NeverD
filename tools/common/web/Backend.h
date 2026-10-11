//===- Backend.h - Shared offline web C API client --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stateful transport adapter; analysis and evidence policy belong to the C
/// API.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "common/transport/Json.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

namespace neverd::web_client {
using transport::Json;

class Backend {
  neverd_web_session_t session_ = nullptr;
  neverd_session_t native_ = nullptr;
  std::string handoffId_;
  std::string revision_ = "0", projectId_, analysisState_ = "not_analyzed";

public:
  Backend() = default;
  ~Backend();
  Backend(const Backend &) = delete;
  Backend &operator=(const Backend &) = delete;
  static Json capabilities();
  Json execute(const std::string &Operation, const Json &Payload);
  const std::string &revision() const { return revision_; }
  const std::string &projectId() const { return projectId_; }
  const std::string &analysisState() const { return analysisState_; }
};
} // namespace neverd::web_client
