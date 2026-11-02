//===- MachOThreadedBind.h - Original arm64e chains ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recognizes original dyld threaded binds before classic streams can publish
/// slots. A recognized stream publishes only after every chain validates.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_MACHO_MACHOTHREADEDBIND_H
#define NEVERD_LIB_LOADER_MACHO_MACHOTHREADEDBIND_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"

#include <cstddef>
#include <cstdint>

namespace neverd::macho_loader::detail {

/// True means the streams were handled, including an explicit image error.
/// False permits the existing classic bind reader to process both streams.
bool parseThreadedBindStreams(const uint8_t *BasePtr, size_t FileSize,
                              const DyldInfoOffsets &Info, va_t TextVMAddr,
                              BinaryImage &Img);

} // namespace neverd::macho_loader::detail

#endif // NEVERD_LIB_LOADER_MACHO_MACHOTHREADEDBIND_H
