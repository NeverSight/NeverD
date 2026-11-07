//===- RenderStore.h - Renders kept between sessions ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// What a replica rendered ahead (EngineRenderAhead.cpp), kept among each
/// user's own project files for later sessions over the same inputs.  Every
/// identity has a folder, and every session appends to a file of its own, so a
/// session that ends at any point leaves each render it finished and no two
/// writers share a file.  Disposable, as the browse cache is: a damaged record
/// is a render not kept, and removing the folder only costs the renders.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_WORKER_RENDERSTORE_H
#define NEVERD_TOOLS_NEVERD_WORKER_RENDERSTORE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace neverd::worker {

class RenderStore {
public:
  /// What earlier sessions kept of \p identity under \p folder.  Only a store
  /// that \p writable may write takes more, and its first write removes what
  /// the folder keeps of other identities.
  RenderStore(std::filesystem::path folder, const std::string &identity,
              bool writable);

  /// Takes up what other sessions have added to the folder since.
  void refresh();
  bool contains(const std::string &key) const { return records_.contains(key); }
  /// The pages kept for \p key.  None for a key kept without pages, and for a
  /// record that no longer reads as it was written, which is forgotten.
  std::vector<std::string> read(const std::string &key);
  /// False when \p pages are not kept, and from then on.
  bool write(const std::string &key, const std::vector<std::string> &pages);
  /// Whether write can still keep anything.
  bool writable() const { return writable_; }
  std::size_t records() const { return records_.size(); }
  /// The bytes of the records kept.
  std::uint64_t bytes() const { return bytes_; }

private:
  struct Record {
    std::size_t file;
    std::uint64_t offset, dataBytes, checksum;
    std::uint32_t pages;
  };
  struct File {
    std::filesystem::path path;
    std::unique_ptr<std::ifstream> input;
    /// Where the next record starts; records before it are known.
    std::uint64_t walked = 0;
  };
  void walkFile(File &file);
  bool startFile();

  std::filesystem::path folder_, directory_;
  bool writable_;
  std::vector<File> files_;
  std::unordered_map<std::string, Record> records_;
  std::uint64_t bytes_ = 0;
  std::ofstream output_;
  std::size_t outputFile_ = 0;
  std::uint64_t outputBytes_ = 0;
};

} // namespace neverd::worker

#endif // NEVERD_TOOLS_NEVERD_WORKER_RENDERSTORE_H
