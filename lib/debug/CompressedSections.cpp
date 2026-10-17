//===- CompressedSections.cpp - Expand compressed ELF sections ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/debug/CompressedSections.h"

#include "neverd/Limits.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/MathExtras.h"

#include <zlib.h>
#ifdef NEVERD_DEBUG_ZSTD
#include <zstd.h>
#endif

#include <cstring>

namespace neverd {

namespace {

/// Expand \p Payload, compressed as \p Type says, into \p Out, which holds
/// the size the compression header states.
bool expandPayload(uint32_t Type, llvm::ArrayRef<uint8_t> Payload,
                   std::vector<uint8_t> &Out) {
  switch (Type) {
  case llvm::ELF::ELFCOMPRESS_ZLIB: {
    uLongf Size = static_cast<uLongf>(Out.size());
    return Size == Out.size() &&
           uncompress(Out.data(), &Size, Payload.data(),
                      static_cast<uLong>(Payload.size())) == Z_OK &&
           Size == Out.size();
  }
#ifdef NEVERD_DEBUG_ZSTD
  case llvm::ELF::ELFCOMPRESS_ZSTD: {
    const size_t Size =
        ZSTD_decompress(Out.data(), Out.size(), Payload.data(), Payload.size());
    return !ZSTD_isError(Size) && Size == Out.size();
  }
#endif
  default:
    return false;
  }
}

template <typename ELFT>
std::optional<std::vector<uint8_t>> expand(llvm::StringRef Bytes) {
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Chdr = typename ELFT::Chdr;
  auto File = llvm::object::ELFFile<ELFT>::create(Bytes);
  if (!File) {
    llvm::consumeError(File.takeError());
    return std::nullopt;
  }
  auto Sections = File->sections();
  if (!Sections) {
    llvm::consumeError(Sections.takeError());
    return std::nullopt;
  }
  const uint64_t HeaderTable = File->getHeader().e_shoff;
  std::vector<uint8_t> Out;
  uint64_t Total = 0;
  for (size_t Index = 0; Index < Sections->size(); ++Index) {
    const Elf_Shdr &Section = (*Sections)[Index];
    if (!(Section.sh_flags & llvm::ELF::SHF_COMPRESSED) ||
        Section.sh_type == llvm::ELF::SHT_NOBITS)
      continue;
    auto Contents = File->getSectionContents(Section);
    if (!Contents) {
      llvm::consumeError(Contents.takeError());
      return std::nullopt;
    }
    if (Contents->size() < sizeof(Elf_Chdr))
      return std::nullopt;
    Elf_Chdr Header;
    std::memcpy(&Header, Contents->data(), sizeof(Header));
    const uint64_t Size = Header.ch_size;
    if (Size > limits::kMaxExpandedDebugSections - Total)
      return std::nullopt;
    Total += Size;
    std::vector<uint8_t> Body(Size);
    if (!expandPayload(Header.ch_type, Contents->drop_front(sizeof(Header)),
                       Body))
      return std::nullopt;
    if (Out.empty())
      Out.assign(Bytes.bytes_begin(), Bytes.bytes_end());
    const uint64_t Alignment = std::max<uint64_t>(Header.ch_addralign, 1);
    const uint64_t At = llvm::alignTo(Out.size(), Alignment);
    // Alignment padding consumes the same bounded expansion budget.
    if (At < Out.size() ||
        At - Out.size() > limits::kMaxExpandedDebugSections - Total ||
        At > Out.max_size() || Size > Out.max_size() - At)
      return std::nullopt;
    Total += At - Out.size();
    Out.resize(At);
    Out.insert(Out.end(), Body.begin(), Body.end());
    // The header table lies inside the copied bytes, at its original place.
    Elf_Shdr Patched;
    const uint64_t HeaderAt = HeaderTable + Index * sizeof(Elf_Shdr);
    std::memcpy(&Patched, Out.data() + HeaderAt, sizeof(Patched));
    Patched.sh_offset = At;
    Patched.sh_size = Size;
    Patched.sh_addralign = Alignment;
    Patched.sh_flags = Section.sh_flags & ~uint64_t(llvm::ELF::SHF_COMPRESSED);
    std::memcpy(Out.data() + HeaderAt, &Patched, sizeof(Patched));
  }
  if (Out.empty())
    return std::nullopt;
  return Out;
}

} // namespace

std::optional<std::vector<uint8_t>>
expandCompressedSections(llvm::StringRef Bytes) {
  if (Bytes.size() < llvm::ELF::EI_NIDENT ||
      !Bytes.starts_with(llvm::ELF::ElfMagic))
    return std::nullopt;
  const bool Is64 = Bytes[llvm::ELF::EI_CLASS] == llvm::ELF::ELFCLASS64;
  const bool Little = Bytes[llvm::ELF::EI_DATA] == llvm::ELF::ELFDATA2LSB;
  if (Is64)
    return Little ? expand<llvm::object::ELF64LE>(Bytes)
                  : expand<llvm::object::ELF64BE>(Bytes);
  return Little ? expand<llvm::object::ELF32LE>(Bytes)
                : expand<llvm::object::ELF32BE>(Bytes);
}

} // namespace neverd
