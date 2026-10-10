//===- BlobStore.cpp - Private immutable blob storage ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private immutable blob storage.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace neverd::web {
struct Blob::Storage {
  int Descriptor = -1;
  bool Sealed = false;
  ~Storage() {
#ifndef _WIN32
    if (Descriptor >= 0)
      close(Descriptor);
#endif
  }
};

Blob::Blob(std::shared_ptr<const Storage> Owner, uint64_t Start,
           uint64_t Length)
    : Owner(std::move(Owner)), Start(Start), Length(Length) {}

Blob Blob::slice(uint64_t Offset, uint64_t Size) const {
  if (Offset > Length || Size > Length - Offset)
    throw Error("invalid_blob_range");
  return Blob(Owner, Start + Offset, Size);
}

std::string Blob::read(uint64_t Offset, uint64_t Size, uint64_t Budget) const {
  if (Offset > Length || Size > Length - Offset)
    throw Error("invalid_blob_range");
  if (Size > Budget || Size > MaxBlobReadBytes)
    throw Error("blob_read_budget_exceeded");
  if (Owner && !Owner->Sealed)
    throw Error("blob_not_sealed");
  if (!Size)
    return {};
#ifdef _WIN32
  throw Error("input_reader_unavailable");
#else
  if (!Owner || Owner->Descriptor < 0)
    throw Error("blob_storage_unavailable");
  std::string Result(size_t(Size), '\0');
  uint64_t Done = 0;
  while (Done < Size) {
    const auto Count = pread(Owner->Descriptor, Result.data() + Done,
                             size_t(std::min(BlobTransferBytes, Size - Done)),
                             off_t(Start + Offset + Done));
    if (Count < 0 && errno == EINTR)
      continue;
    if (Count <= 0)
      throw Error("blob_storage_read_failed");
    Done += uint64_t(Count);
  }
  return Result;
#endif
}

std::string Blob::digest() const {
  if (Owner && !Owner->Sealed)
    throw Error("blob_not_sealed");
  llvm::SHA256 Hash;
  for (uint64_t Offset = 0; Offset < Length;) {
    const auto Size = std::min(BlobTransferBytes, Length - Offset);
    Hash.update(llvm::StringRef(read(Offset, Size)));
    Offset += Size;
  }
  return llvm::toHex(Hash.final(), true);
}

BlobStore::BlobStore(uint64_t Budget) : Budget(Budget) {
  if (!Budget || Budget > Limits::HardInputBytes)
    throw Error("invalid_blob_store_budget");
}

BlobStore::Captured BlobStore::capture(int Descriptor, uint64_t ExpectedSize) {
  if (Finished || Failed)
    throw Error("blob_store_closed");
  if (ExpectedSize > Budget - Used)
    throw Error("budget_exceeded");
#ifdef _WIN32
  throw Error("input_reader_unavailable");
#else
  static_assert(sizeof(off_t) >= sizeof(int64_t));
  static_assert(Limits::HardInputBytes <
                uint64_t(std::numeric_limits<off_t>::max()));
  try {
    if (!State) {
      State = std::make_shared<Blob::Storage>();
      llvm::SmallString<128> Model, Path;
      llvm::sys::path::system_temp_directory(true, Model);
      llvm::sys::path::append(
          Model, "neverd-web-spool-%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%.tmp");
      // Request exclusive creation, mode 0600 and close-on-exec. Do not use
      // createTemporaryFile: this LLVM revision uses 0666 despite its header's
      // 0600 claim. Explicit permissions are part of the storage boundary.
      // Remove the directory entry before copying any target bytes. No name
      // survives in the store, output, crash cleanup list or analysis model.
      if (llvm::sys::fs::createUniqueFile(Model, State->Descriptor, Path,
                                          llvm::sys::fs::OF_None, 0600))
        throw Error("blob_storage_unavailable");
      if (unlink(Path.c_str()) != 0) {
        // No input bytes have been written. Best-effort remove the empty file.
        llvm::sys::fs::remove(Path);
        throw Error("blob_storage_unavailable");
      }
      struct stat Info{};
      const int Flags = fcntl(State->Descriptor, F_GETFD);
      if (fstat(State->Descriptor, &Info) != 0 || !S_ISREG(Info.st_mode) ||
          Info.st_nlink != 0 || (Info.st_mode & 0777) != 0600 || Flags < 0 ||
          !(Flags & FD_CLOEXEC))
        throw Error("blob_storage_unavailable");
    }
    std::array<char, BlobTransferBytes> Buffer;
    llvm::SHA256 Hash;
    uint64_t Done = 0;
    while (Done < ExpectedSize) {
      const auto Count =
          pread(Descriptor, Buffer.data(),
                size_t(std::min(BlobTransferBytes, ExpectedSize - Done)),
                off_t(Done));
      if (Count < 0 && errno == EINTR)
        continue;
      if (Count <= 0)
        throw Error("input_changed");
      uint64_t Written = 0;
      while (Written < uint64_t(Count)) {
        const auto N = pwrite(State->Descriptor, Buffer.data() + Written,
                              size_t(uint64_t(Count) - Written),
                              off_t(Used + Done + Written));
        if (N < 0 && errno == EINTR)
          continue;
        if (N <= 0)
          throw Error("blob_storage_write_failed");
        Written += uint64_t(N);
      }
      Hash.update(llvm::StringRef(Buffer.data(), size_t(Count)));
      Done += uint64_t(Count);
    }
    char Extra;
    ssize_t Count;
    do {
      Count = pread(Descriptor, &Extra, 1, off_t(ExpectedSize));
    } while (Count < 0 && errno == EINTR);
    if (Count != 0)
      throw Error("input_changed");
    Captured Result{Blob(State, Used, ExpectedSize),
                    llvm::toHex(Hash.final(), true)};
    Used += ExpectedSize;
    return Result;
  } catch (...) {
    Failed = true;
    throw;
  }
#endif
}

void BlobStore::seal() {
  if (Finished || Failed)
    throw Error("blob_store_closed");
  Finished = true;
#ifndef _WIN32
  if (State) {
    struct stat Info{};
    if (fstat(State->Descriptor, &Info) != 0 || Info.st_size < 0 ||
        uint64_t(Info.st_size) != Used || fsync(State->Descriptor) != 0 ||
        fchmod(State->Descriptor, 0400) != 0)
      throw Error("blob_storage_write_failed");
    State->Sealed = true;
  }
#endif
}
} // namespace neverd::web
