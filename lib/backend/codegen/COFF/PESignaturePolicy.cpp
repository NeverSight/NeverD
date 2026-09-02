//===- PESignaturePolicy.cpp - Strict PE signing policy ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/COFF/PESignaturePolicy.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <algorithm>
#include <cstdint>

namespace neverd::pe_signature {

namespace {

llvm::Error malformed(llvm::StringRef Detail) {
  return llvm::make_error<llvm::StringError>("pe signature: " + Detail,
                                             llvm::inconvertibleErrorCode());
}

bool rangeInBounds(uint64_t Offset, uint64_t Size, uint64_t Total) {
  return Offset <= Total && Size <= Total - Offset;
}

} // namespace

llvm::Expected<Profile> inspect(llvm::ArrayRef<uint8_t> Binary) {
  const llvm::StringRef Bytes(reinterpret_cast<const char *>(Binary.data()),
                              Binary.size());
  auto Object = llvm::object::COFFObjectFile::create(
      llvm::MemoryBufferRef(Bytes, "<memory>"));
  if (!Object)
    return Object.takeError();
  if (!(*Object)->getPE32Header() && !(*Object)->getPE32PlusHeader())
    return malformed("input is not a PE image");

  const llvm::object::data_directory *Security =
      (*Object)->getDataDirectory(llvm::COFF::CERTIFICATE_TABLE);
  if (!Security)
    return Profile{};
  const uint32_t Offset = Security->RelativeVirtualAddress;
  const uint32_t Size = Security->Size;
  if (Offset == 0 && Size == 0)
    return Profile{};
  if (Offset == 0 || Size == 0)
    return malformed("certificate table offset and size must both be zero");
  if ((Offset & 7u) != 0)
    return malformed("certificate table offset must be 8-byte aligned");
  if (!rangeInBounds(Offset, Size, Binary.size()))
    return malformed("certificate table is outside the file");

  constexpr uint64_t HeaderSize = 8;
  uint64_t Cursor = Offset;
  const uint64_t End = uint64_t(Offset) + Size;
  uint32_t Count = 0;
  while (Cursor < End) {
    const uint64_t Remaining = End - Cursor;
    if (Remaining < HeaderSize)
      return malformed("WIN_CERTIFICATE header is truncated");
    const uint32_t Length =
        llvm::support::endian::read32le(Binary.data() + Cursor);
    if (Length < HeaderSize || Length > Remaining)
      return malformed("WIN_CERTIFICATE length is invalid");
    const uint64_t AlignedLength = (uint64_t(Length) + 7) & ~uint64_t(7);
    if (AlignedLength > Remaining)
      return malformed("WIN_CERTIFICATE aligned record exceeds the table");
    if (!std::all_of(Binary.begin() + Cursor + Length,
                     Binary.begin() + Cursor + AlignedLength,
                     [](uint8_t Byte) { return Byte == 0; }))
      return malformed("WIN_CERTIFICATE alignment padding is not zero");
    Cursor += AlignedLength;
    ++Count;
  }

  Profile Result;
  Result.SignatureKind = Kind::Authenticode;
  Result.CertificateTableOffset = Offset;
  Result.CertificateTableSize = Size;
  Result.CertificateCount = Count;
  return Result;
}

} // namespace neverd::pe_signature
