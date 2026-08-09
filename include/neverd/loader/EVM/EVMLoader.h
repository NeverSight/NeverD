//===- EVMLoader.h - Ethereum Virtual Machine bytecode loader -*- C++ -*-===//

#ifndef NEVERD_LOADER_EVM_EVMLOADER_H
#define NEVERD_LOADER_EVM_EVMLOADER_H

#include "neverd/loader/BinaryImage.h"

namespace neverd {

class EVMLoader final : public Loader {
public:
  llvm::Expected<BinaryImage> load(const std::filesystem::path &Path) override;
};

} // namespace neverd

#endif // NEVERD_LOADER_EVM_EVMLOADER_H
