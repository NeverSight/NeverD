//===- ViewStorage.h - Temporary source document records -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Disk records shared by retained source documents. Paging reads selected
/// chunks; releasing the last owner closes and removes the temporary file.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WORKER_VIEWSTORAGE_H
#define NEVERD_WORKER_VIEWSTORAGE_H

#include "Protocol.h"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace neverd::worker {

class ViewStorage {
public:
  struct Record {
    std::size_t offset = 0, size = 0;
  };

  ViewStorage() : file_(std::tmpfile(), &std::fclose) {
    if (!file_)
      throw Error("io_error", "Cannot create temporary source storage");
  }

  Record append(std::string_view text) {
    seek(end_);
    if (std::fwrite(text.data(), 1, text.size(), file_.get()) != text.size())
      throw Error("io_error", "Cannot write temporary source storage");
    Record record{end_, text.size()};
    end_ += text.size();
    return record;
  }

  std::string read(Record record) const {
    seek(record.offset);
    std::string text(record.size, '\0');
    if (std::fread(text.data(), 1, text.size(), file_.get()) != text.size())
      throw Error("io_error", "Cannot read temporary source storage");
    return text;
  }

private:
  void seek(std::size_t offset) const {
#ifdef _WIN32
    const int result =
        _fseeki64(file_.get(), static_cast<__int64>(offset), SEEK_SET);
#else
    const int result =
        fseeko(file_.get(), static_cast<off_t>(offset), SEEK_SET);
#endif
    if (result != 0)
      throw Error("io_error", "Cannot seek temporary source storage");
  }
  std::unique_ptr<FILE, decltype(&std::fclose)> file_;
  std::size_t end_ = 0;
};

} // namespace neverd::worker

#endif // NEVERD_WORKER_VIEWSTORAGE_H
