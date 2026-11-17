//===- Manifest.h - Shared process plugin declarations --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Normalized v1 declarations, configuration and structured result validation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_MANIFEST_H
#define NEVERD_PLUGIN_MANIFEST_H

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::plugin {
using Json = nlohmann::json;
bool validIdentifier(std::string_view Value);
/// Parses JSON from a provider, manifest or consumer. Malformed text and
/// nesting deeper than limits::kMaxJsonDepth return a discarded value.
Json parseJson(std::string_view Text);

struct CapabilityRequirement {
  std::string Id;
  uint32_t Version = 1;
};

struct Manifest {
  std::string Id, Name, Version, Icon;
  std::filesystem::path Path, Program;
  std::vector<std::string> Arguments, Queries;
  std::vector<CapabilityRequirement> Services, Events;
  Json Actions;
  // Native GUI packages are discovered here but loaded only by the GUI host.
  std::filesystem::path GuiLibrary;
};

std::optional<Manifest> loadManifest(const std::filesystem::path &Path,
                                     std::string &Error);
bool validateParameterFields(const Json &Fields, std::string &Error);
bool validateParameters(const Json &Fields, const Json &Values,
                        std::string &Error);
/// One resolver for all consumers. Each layer is a partial object; absent
/// values inherit, explicit null is checked as a value. Arrays replace whole
/// values. No layer or stored preference is mutated by resolution failure.
std::optional<Json> resolveParameters(const Json &Fields, const Json &User,
                                      const Json &Project, const Json &Explicit,
                                      std::string &Error);
bool validateResult(const Json &Result, std::string &Error,
                    bool StructuredData = false);
std::vector<std::string> queryServices();
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_MANIFEST_H
