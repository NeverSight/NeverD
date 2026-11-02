//===- PatchExport.cpp - Exclusive patched binary export ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Replay original bytes, handle signing metadata, publish a new file.
//===----------------------------------------------------------------------===//

#include "neverd/loader/BinaryImage.h"
#include "neverd/object/PEChecksum.h"
#include "neverd/patch/PatchIO.h"
#include "neverd/support/AtomicOutput.h"
#include "neverd/support/FilePath.h"

#include "llvm/Object/COFF.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>

using namespace neverd;
using namespace neverd::patch;

static llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::make_error<llvm::StringError>(Message,
                                             llvm::inconvertibleErrorCode());
}

llvm::Error patch::exportPatches(const BinaryImage &Img, const InputFile &Input,
                                 const PatchOverlay &Overlay,
                                 const std::filesystem::path &Output,
                                 bool AllowUnsigned) {
  auto Data = Input.read(0, Input.size());
  if (!Data)
    return Data.takeError();
  std::optional<uint64_t> Checksum;
  std::optional<uint64_t> Security;
  std::optional<uint64_t> MachSignature;
  uint64_t MachCommandsEnd = 0;
  uint32_t MachCommands = 0, MachCommandBytes = 0, MachSignatureBytes = 0;
  const auto Buffer = [&] {
    return llvm::MemoryBufferRef(
        llvm::StringRef(reinterpret_cast<const char *>(Data->data()),
                        Data->size()),
        "patch export");
  };
  if (Img.Format == BinaryFormat::COFF) {
    auto Object = llvm::object::COFFObjectFile::create(Buffer());
    if (!Object)
      return Object.takeError();
    const auto Offset = [&](const void *P) {
      return static_cast<const uint8_t *>(P) - Data->data();
    };
    if (auto *Header = (*Object)->getPE32Header()) {
      if (Header->CheckSum)
        Checksum = Offset(&Header->CheckSum);
    } else if (auto *Header = (*Object)->getPE32PlusHeader()) {
      if (Header->CheckSum)
        Checksum = Offset(&Header->CheckSum);
    }
    if (auto *Directory =
            (*Object)->getDataDirectory(llvm::COFF::CERTIFICATE_TABLE))
      if (Directory->Size || Directory->RelativeVirtualAddress)
        Security = Offset(Directory);
  } else if (Img.Format == BinaryFormat::MachO) {
    auto Whole = Buffer().getBuffer();
    auto Object = llvm::object::ObjectFile::createObjectFile(
        llvm::MemoryBufferRef(Whole.drop_front(Input.base()), "Mach-O slice"));
    if (!Object)
      return Object.takeError();
    auto *Mach = llvm::dyn_cast<llvm::object::MachOObjectFile>(Object->get());
    if (!Mach || !Mach->isLittleEndian())
      return invalid("unsupported Mach-O patch export byte order");
    MachCommands = Mach->getHeader().ncmds;
    MachCommandBytes = Mach->getHeader().sizeofcmds;
    MachCommandsEnd = Input.base() + MachCommandBytes +
                      (Mach->is64Bit() ? sizeof(llvm::MachO::mach_header_64)
                                       : sizeof(llvm::MachO::mach_header));
    for (const auto &Command : Mach->load_commands()) {
      if (Command.C.cmd != llvm::MachO::LC_CODE_SIGNATURE)
        continue;
      if (MachSignature)
        return invalid("duplicate Mach-O code signatures");
      MachSignature =
          reinterpret_cast<const uint8_t *>(Command.Ptr) - Data->data();
      MachSignatureBytes = Command.C.cmdsize;
    }
  }
  if ((Security || MachSignature) && !AllowUnsigned)
    return invalid(
        "input is signed; explicitly enable allow_unsigned to export");
  for (const auto &[Offset, Row] : Overlay.rows()) {
    if (Offset > Data->size() || Row.size() > Data->size() - Offset ||
        !std::equal(Row.Original.begin(), Row.Original.end(),
                    Data->begin() + Offset))
      return invalid("patch originals do not match the export input");
    // Format metadata is owned by the loader. Interactive byte changes must
    // never silently collide with the exporter's checksum/signing edits.
    const auto Hits = [&](uint64_t A, uint64_t N) {
      return Offset < A + N && A < Offset + Row.size();
    };
    if ((Checksum && Hits(*Checksum, sizeof(uint32_t))) ||
        (Security && Hits(*Security, sizeof(llvm::object::data_directory))) ||
        (MachSignature && Hits(Input.base(), MachCommandsEnd - Input.base())))
      return invalid(
          "patch overlaps export-owned checksum or signature metadata");
    std::copy(Row.Current.begin(), Row.Current.end(), Data->begin() + Offset);
  }
  if (Security)
    std::fill_n(Data->begin() + *Security, sizeof(llvm::object::data_directory),
                0);
  if (MachSignature) {
    const uint64_t Next = *MachSignature + MachSignatureBytes;
    if (Next > MachCommandsEnd || MachCommandsEnd > Data->size() ||
        !MachCommands)
      return invalid("invalid Mach-O signature command extent");
    std::memmove(Data->data() + *MachSignature, Data->data() + Next,
                 MachCommandsEnd - Next);
    std::fill(Data->begin() + MachCommandsEnd - MachSignatureBytes,
              Data->begin() + MachCommandsEnd, 0);
    llvm::support::endian::write32le(
        Data->data() + Input.base() + offsetof(llvm::MachO::mach_header, ncmds),
        MachCommands - 1);
    llvm::support::endian::write32le(
        Data->data() + Input.base() +
            offsetof(llvm::MachO::mach_header, sizeofcmds),
        MachCommandBytes - MachSignatureBytes);
  }
  if (Checksum) {
    auto Sum = computePEChecksum(*Data, *Checksum);
    if (!Sum)
      return invalid("cannot compute PE checksum");
    llvm::support::endian::write32le(Data->data() + *Checksum, *Sum);
  }
  // Reparse the product before publication. A byte overlay cannot publish a
  // malformed container even if the original container was valid.
  if (Img.Format == BinaryFormat::COFF) {
    auto Product = llvm::object::COFFObjectFile::create(Buffer());
    if (!Product)
      return Product.takeError();
  } else if (Img.Format == BinaryFormat::MachO) {
    auto Product =
        llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(
            Buffer().getBuffer().drop_front(Input.base()), "patched Mach-O"));
    if (!Product)
      return Product.takeError();
  }
  const auto Name = pathToUTF8(Output);
  auto Temporary = llvm::sys::fs::TempFile::create(Name + ".tmp-%%%%%%");
  if (!Temporary)
    return Temporary.takeError();
  llvm::raw_fd_ostream Stream(Temporary->FD, false);
  Stream.write(reinterpret_cast<const char *>(Data->data()), Data->size());
  Stream.flush();
  if (Stream.has_error()) {
    const auto EC = Stream.error();
    Stream.clear_error();
    llvm::consumeError(Temporary->discard());
    return llvm::errorCodeToError(EC);
  }
  // Retire delete-on-close before publication on Windows. CreateNew keeps
  // existing destinations, including a symlink to the input, unchanged.
  return support::atomic_output::closeAndCommitTemporaryOutput(
      *Temporary, Name, support::atomic_output::OutputMode::CreateNew,
      Input.permissions());
}
