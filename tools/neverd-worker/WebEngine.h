#pragma once

#include "Protocol.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

namespace neverd::worker {
/// A transport adapter only. All analysis decisions belong to the C API.
class WebEngine {
  neverd_web_session_t session_ = nullptr;
  neverd_session_t native_ = nullptr;
  std::string handoffId_;
  std::string revision_ = "0", projectId_, analysisState_ = "not_analyzed";

public:
  WebEngine() = default;
  ~WebEngine();
  WebEngine(const WebEngine &) = delete;
  WebEngine &operator=(const WebEngine &) = delete;
  static Json capabilities();
  Json execute(const std::string &operation, const Json &payload);
  const std::string &revision() const { return revision_; }
  const std::string &projectId() const { return projectId_; }
  const std::string &analysisState() const { return analysisState_; }
};
} // namespace neverd::worker
