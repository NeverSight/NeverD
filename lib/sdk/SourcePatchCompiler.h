//===- SourcePatchCompiler.h - Source compiler adapters -----------*- C++
//-*-===//

#ifndef NEVERD_SDK_SOURCEPATCHCOMPILER_H
#define NEVERD_SDK_SOURCEPATCHCOMPILER_H

#include "neverd/Common.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/sdk/NeverDCAPIPatch.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd {
struct BinaryImage;
}

namespace neverd::sdk {

struct SourcePatchCompilation {
  std::string Bytes;
  std::string CompilerPath;
  std::string CompilerVersion;
  std::vector<std::string> Arguments;
  std::string Diagnostics;
  std::optional<DynCodePatchEvidence> DynCode;
};

llvm::Expected<SourcePatchCompilation>
compileSourcePatch(llvm::StringRef Source,
                   const neverd_source_patch_options_v1 &Options,
                   const BinaryImage &Image);

} // namespace neverd::sdk

#endif // NEVERD_SDK_SOURCEPATCHCOMPILER_H
