//===- DataInCode.h - Load Darwin embedded data ranges ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_MACHO_DATAINCODE_H
#define NEVERD_LIB_LOADER_MACHO_DATAINCODE_H

#include "neverd/loader/MachO/DataInCode.h"

#include "llvm/BinaryFormat/MachO.h"

namespace neverd {
struct BinaryImage;
llvm::Error
loadMachODataInCode(BinaryImage &Image,
                    const llvm::MachO::linkedit_data_command &Command,
                    llvm::endianness Order);
} // namespace neverd
#endif
