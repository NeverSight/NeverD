//===- ZipPayload.cpp - Bounded ZIP member decoding -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// In-process stored/raw-deflate decoding with exact size and CRC validation.
///
//===----------------------------------------------------------------------===//

#include "../BlobStore.h"
#include "ZipInternal.h"

#include "neverd/web/Error.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/CRC.h"

#include <algorithm>
#include <array>
#ifdef NEVERD_WEB_ZLIB
#include <zlib.h>
#endif

namespace neverd::web {
bool packageZipDeflateAvailable() {
#ifdef NEVERD_WEB_ZLIB
  return true;
#else
  return false;
#endif
}

void decodeZipMember(const Blob &Stored, uint16_t Method, uint64_t Size,
                     uint32_t ExpectedCRC, BlobStore &Store, uint64_t &Steps) {
  uint64_t Read = 0, Produced = 0;
  uint32_t CRC = 0;
  const auto Step = [&] {
    if (++Steps > 4 * MaxPackageArchiveBytes / BlobTransferBytes +
                      2 * MaxPackageArchiveMembers)
      throw Error("zip_work_budget_exceeded");
  };
  const auto Append = [&](std::string_view Bytes) {
    if (Bytes.size() > Size - Produced)
      throw Error("zip_uncompressed_size_mismatch");
    Produced += Bytes.size();
    CRC = llvm::crc32(
        CRC, {reinterpret_cast<const uint8_t *>(Bytes.data()), Bytes.size()});
    Store.append(Bytes);
  };
  if (Method == 0) {
    if (Stored.size() != Size)
      throw Error("zip_stored_size_mismatch");
    while (Read < Stored.size()) {
      Step();
      auto Bytes =
          Stored.read(Read, std::min(BlobTransferBytes, Stored.size() - Read));
      Read += Bytes.size();
      Append(Bytes);
    }
  } else if (Method == 8) {
#ifndef NEVERD_WEB_ZLIB
    throw Error("zip_deflate_unavailable");
#else
    struct Inflater {
      z_stream Stream{};
      Inflater() {
        if (inflateInit2(&Stream, -15) != Z_OK)
          throw Error("zip_deflate_unavailable");
      }
      ~Inflater() { inflateEnd(&Stream); }
    } Decoder;
    auto &S = Decoder.Stream;
    std::string Input;
    std::array<char, BlobTransferBytes> Output;
    for (;;) {
      Step();
      if (!S.avail_in && Read < Stored.size()) {
        Input = Stored.read(Read,
                            std::min(BlobTransferBytes, Stored.size() - Read));
        Read += Input.size();
        S.next_in = reinterpret_cast<Bytef *>(Input.data());
        S.avail_in = uInt(Input.size());
      }
      const auto Before = S.avail_in;
      S.next_out = reinterpret_cast<Bytef *>(Output.data());
      S.avail_out = uInt(Output.size());
      const int Status = inflate(&S, Z_NO_FLUSH);
      const auto Count = Output.size() - S.avail_out;
      Append({Output.data(), Count});
      if (Status == Z_STREAM_END) {
        if (S.avail_in || Read != Stored.size())
          throw Error("zip_trailing_deflate_data");
        break;
      }
      if (Status != Z_OK || (!Count && Before == S.avail_in))
        throw Error("zip_invalid_deflate");
    }
#endif
  } else {
    throw Error("zip_unsupported_compression");
  }
  if (Produced != Size)
    throw Error("zip_uncompressed_size_mismatch");
  if (CRC != ExpectedCRC)
    throw Error("zip_crc_mismatch");
}
} // namespace neverd::web
