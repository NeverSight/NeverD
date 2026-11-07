//===- Schema.h - Structured action result contracts -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A strict JSON schema subset for host action results. Unknown keywords fail.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_SCHEMA_H
#define NEVERD_PLUGIN_SCHEMA_H

#include "neverd/plugin/Manifest.h"

#include <string>

namespace neverd::plugin {
bool validateSchema(const Json &Schema, std::string &Error);
bool validateValue(const Json &Schema, const Json &Value, std::string &Error);
/// Project the existing v1 fields without changing their resolution rules.
Json parameterSchema(const Json &Fields);
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_SCHEMA_H
