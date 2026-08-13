//===- BytecodeDetail.h - Private EVM bytecode helpers ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_EVM_BYTECODEDETAIL_H
#define NEVERD_LIB_EVM_BYTECODEDETAIL_H

#include "neverd/evm/Bytecode.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::evm::detail {

llvm::Error inputError(llvm::StringRef SourceName, llvm::Twine Message);

struct DecodedArtifact {
  std::vector<uint8_t> Code;
  bool IsRuntime = false;
};

llvm::Expected<std::vector<uint8_t>>
decodeHexBytecode(llvm::StringRef Content, llvm::StringRef SourceName);

llvm::Expected<DecodedArtifact>
decodeArtifactBytecode(llvm::StringRef Content,
                       const BytecodeLoadOptions &Options,
                       llvm::StringRef SourceName);

std::optional<std::vector<uint8_t>>
extractStaticRuntime(llvm::ArrayRef<uint8_t> Code, Hardfork Fork);

} // namespace neverd::evm::detail

#endif // NEVERD_LIB_EVM_BYTECODEDETAIL_H
