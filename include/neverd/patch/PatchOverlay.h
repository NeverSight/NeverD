//===- PatchOverlay.h - Interactive byte patches over an image -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The byte overlay interactive patches write into a loaded image.  It owns
/// whether bytes may change (file-backed, outside relocated fields, still as
/// the input file holds them), writes the image's segment and section bytes
/// so decoding and analysis read the patched bytes, keeps the input file's
/// bytes for every change so a revert restores them exactly, and keys every
/// change by its offset in the input file so a project and an export replay
/// it onto the original file.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PATCH_PATCHOVERLAY_H
#define NEVERD_PATCH_PATCHOVERLAY_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

struct BinaryImage;

namespace patch {

enum class PatchKind : uint8_t {
#define NEVERD_PATCH_KIND(Id, Spelling) Id,
#include "neverd/patch/PatchKinds.def"
};

/// The kind's spelling in requests and patches.json (PatchKinds.def).
llvm::StringRef patchKindName(PatchKind Kind);
std::optional<PatchKind> parsePatchKind(llvm::StringRef Spelling);

/// One run of changed bytes: contiguous in the input file and inside one
/// segment of the image, of one kind, every byte different from the input
/// file's.  Adjacent changes of the same kind form one run.
struct PatchRow {
  /// Where the run starts in the input file (a universal Mach-O's slice
  /// offset included) and in the image.
  uint64_t FileOffset = 0;
  va_t Address = 0;
  /// The input file's bytes and the patched bytes, of equal length.
  std::vector<uint8_t> Original;
  std::vector<uint8_t> Current;
  PatchKind Kind = PatchKind::Bytes;

  uint64_t size() const { return Current.size(); }
  va_t end() const { return Address + Current.size(); }
  bool operator==(const PatchRow &) const = default;
};

/// The file an image was loaded from, read again for the bytes the loader
/// took from it.
class InputFile {
public:
  /// \p Path as \p Img's input.  A universal Mach-O places the image at the
  /// offset of the slice its loader reads; a dyld shared cache image is
  /// refused, its file offsets naming several files.
  static llvm::Expected<InputFile> open(const std::filesystem::path &Path,
                                        const BinaryImage &Img);

  const std::filesystem::path &path() const { return Path; }
  uint64_t size() const { return Size; }
  std::filesystem::perms permissions() const { return Permissions; }
  /// The file offset of the image's file offset zero.
  uint64_t base() const { return Base; }
  bool overlapsLoadHeaders(uint64_t Offset, uint64_t Count) const {
    return HeaderSize && Count &&
           (Offset <= Base ? Base - Offset < Count
                           : Offset - Base < HeaderSize);
  }
  /// Immutable \p Size bytes at file offset \p Offset in the captured input.
  /// The view remains valid while this input (or a copy retaining the same
  /// snapshot) is alive.
  llvm::Expected<llvm::ArrayRef<uint8_t>> view(uint64_t Offset,
                                               uint64_t Size) const;
  /// Owning copy of \p Size bytes from \p Offset, an offset in the file.
  llvm::Expected<std::vector<uint8_t>> read(uint64_t Offset,
                                            uint64_t Size) const;

private:
  std::filesystem::path Path;
  uint64_t Size = 0;
  uint64_t Base = 0;
  uint64_t HeaderSize = 0;
  std::filesystem::perms Permissions = std::filesystem::perms::unknown;
  std::shared_ptr<const llvm::MemoryBuffer> Snapshot;
};

/// Where a write lands.
struct WriteTarget {
  size_t Segment = 0;
  /// The write's first byte in the input file.
  uint64_t FileOffset = 0;
};

/// The patches over one loaded image.  Every operation either completes or
/// leaves the image and the overlay as they were.
class PatchOverlay {
public:
  /// Whether \p Size bytes at \p Address may change: they lie in one
  /// segment's file-backed bytes, no relocation or fixup field of the
  /// loader reaches into them, and those the overlay has not changed are
  /// still the input file's bytes (a loader that rewrote them, as an object
  /// file's relocations do, leaves no input byte to restore).  The error
  /// names the first reason.
  llvm::Expected<WriteTarget> admit(const BinaryImage &Img,
                                    const InputFile &Input, va_t Address,
                                    uint64_t Size) const;

  /// Write \p Bytes at \p Address into \p Img as a change of \p Kind.
  llvm::Error apply(BinaryImage &Img, const InputFile &Input, va_t Address,
                    llvm::ArrayRef<uint8_t> Bytes, PatchKind Kind);

  /// Restore the changed bytes in [\p Address, \p Address + \p Size) to the
  /// input file's; a zero \p Size restores the run holding \p Address.
  /// Returns how many bytes changed back, an error when none had changed.
  llvm::Expected<uint64_t> revert(BinaryImage &Img, va_t Address,
                                  uint64_t Size);

  /// Restore every changed byte.
  void clear(BinaryImage &Img);

  /// Replace the overlay with \p Rows, as a project's patches.json holds
  /// them: each row must still describe \p Input and \p Img.  All or none.
  llvm::Error replace(BinaryImage &Img, const InputFile &Input,
                      std::vector<PatchRow> Rows);

  /// The runs by their offset in the input file.
  const std::map<uint64_t, PatchRow> &rows() const { return Rows; }
  bool empty() const { return Rows.empty(); }
  /// Changed bytes in all runs.
  uint64_t bytes() const { return Bytes; }

  /// The runs that hold a byte of [\p Address, \p Address + \p Size).
  std::vector<const PatchRow *> rowsIn(va_t Address, uint64_t Size) const;
  /// The input file's byte at \p Address when the overlay changed it.
  std::optional<uint8_t> originalByte(va_t Address) const;
  /// Replace the changed bytes of \p Data, the image's bytes from
  /// \p Address, with the input file's: what readers that must not see the
  /// patches (library signatures) read.
  void restoreOriginal(va_t Address, llvm::MutableArrayRef<uint8_t> Data) const;
  /// Whether \p Address .. \p Address + \p Size holds a changed byte.
  bool changes(va_t Address, uint64_t Size) const {
    return !rowsIn(Address, Size).empty();
  }

private:
  std::map<uint64_t, PatchRow> Rows;
  /// Run start addresses to their file offsets.
  std::map<va_t, uint64_t> ByAddress;
  uint64_t Bytes = 0;
};

/// The relocation or fixup field of \p Img that reaches into
/// [\p Address, \p Address + \p Size), as "<what> at 0x..."; none when no
/// field does.  Widths a loader does not record count as pointer-sized.
std::optional<std::string> relocatedFieldIn(const BinaryImage &Img,
                                            va_t Address, uint64_t Size);

/// Write \p Bytes at \p Address into every copy \p Img keeps of those bytes:
/// its segment's data and the data of each section that holds them.
void writeImageBytes(BinaryImage &Img, va_t Address,
                     llvm::ArrayRef<uint8_t> Bytes);

} // namespace patch
} // namespace neverd

#endif // NEVERD_PATCH_PATCHOVERLAY_H
