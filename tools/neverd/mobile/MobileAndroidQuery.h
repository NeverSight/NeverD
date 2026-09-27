//===- MobileAndroidQuery.h - Bounded Android bytecode queries ------------===//
#pragma once

#include "MobileCommon.h"
#include "MobileDalvik.h"

namespace neverd::mobile {
// The visitor receives one complete, validated selected payload at a time.
// Names are APK entry names or the raw DEX file's basename.
void visitAndroidDexes(
    const fs::path &input, Budget &budget,
    const std::function<void(std::string_view, std::string_view)> &visit);

// Validate every identity before filtering or publishing a query result.
void validateAndroidClassIdentities(const std::vector<std::string> &classes,
                                    Budget &budget);

dalvik::DexReferenceKind androidReferenceKind(std::string_view name);
std::string_view androidReferenceKindName(dalvik::DexReferenceKind kind);
llvm::json::Object findAndroidReferences(const fs::path &input,
                                         const dalvik::DexReferenceQuery &query,
                                         Budget &budget);
} // namespace neverd::mobile
