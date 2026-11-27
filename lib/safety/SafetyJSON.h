//===- SafetyJSON.h - Shared finding serialization --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Serialize the finding carried by one scan observation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SAFETY_SAFETYJSON_H
#define NEVERD_LIB_SAFETY_SAFETYJSON_H

#include "neverd/safety/SafetyTypes.h"

#include "llvm/Support/JSON.h"

namespace neverd::safety {
llvm::json::Object findingJson(const Finding &F);
}

#endif // NEVERD_LIB_SAFETY_SAFETYJSON_H
