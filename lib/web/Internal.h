#pragma once

#include "neverd/web/Artifact.h"
#include "neverd/web/Limits.h"
#include "neverd/web/Session.h"

#include "llvm/Support/JSON.h"

namespace neverd::web {

Snapshot capture(std::string_view Path, const Limits &Budget);
std::string json(llvm::json::Value Value);
Limits parseLimits(std::string_view Options);
std::string limitsIdentity(const Limits &Budget);
void validateMemberName(std::string_view Name);
std::string nameKey(std::string_view Name);

} // namespace neverd::web
