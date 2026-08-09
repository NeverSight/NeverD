//===- EVMLoader.h - Ethereum Virtual Machine bytecode loader -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the adapter from normalized EVM bytecode to NeverD's BinaryImage.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EVM_EVMLOADER_H
#define NEVERD_LOADER_EVM_EVMLOADER_H

#include "neverd/loader/BinaryImage.h"

namespace neverd {

/// Loads EVM containers through the shared normalizer and constructs the
/// synthetic executable code section used by the generic pipeline.
class EVMLoader final : public Loader {
public:
  llvm::Expected<BinaryImage> load(const std::filesystem::path &Path) override;
};

} // namespace neverd

#endif // NEVERD_LOADER_EVM_EVMLOADER_H
