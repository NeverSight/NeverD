//===- SeekableZip.h - Seekable ZIP member inspection ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate a checkpoint directory and decode only selected members.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_SEEKABLEZIP_H
#define NEVERD_WEB_SEEKABLEZIP_H
#include "neverd/web/PackageArchive.h"

namespace neverd::web {
struct CheckpointZipMember {
  PackageArchiveMember Record;
  uint32_t CRC;
};
/// Seekable checkpoint inventory. Validate both directories without reading
/// storage slabs. Member CRCs apply to independently decoded ranges.
using ZipReadAt = llvm::function_ref<std::string(uint64_t, uint64_t)>;
std::vector<CheckpointZipMember>
readCheckpointZipDirectory(uint64_t Size, ZipReadAt ReadAt,
                           llvm::function_ref<bool()> Stop);
void decodeZipMemberRange(uint64_t StoredSize, ZipReadAt ReadAt,
                          uint16_t Method, uint64_t Size, uint32_t ExpectedCRC,
                          llvm::function_ref<void(std::string_view)> Append,
                          uint64_t &Steps, llvm::function_ref<bool()> Stop);
} // namespace neverd::web
#endif // NEVERD_WEB_SEEKABLEZIP_H
