//===- SBFELFLoader.h - Solana SBF ELF loader ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_SBFELFLOADER_H
#define NEVERD_LOADER_ELF_SBFELFLOADER_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

namespace neverd {

/// Parse \p Image as Solana SBF when its ELF machine is EM_BPF or EM_SBPF.
/// Returns false without modifying the image for every other ELF machine.
llvm::Expected<bool> loadSBFELF(BinaryImage &Image);

} // namespace neverd

#endif // NEVERD_LOADER_ELF_SBFELFLOADER_H
