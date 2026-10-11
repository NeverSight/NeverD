//===- InputRoot.cpp - Confined input traversal ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Confined input traversal.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"
#include "Internal.h"

#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Unicode.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <set>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace neverd::web {

void validateMemberName(std::string_view Name) {
  if (Name.empty() || Name == "." || Name == ".." || Name.size() > 255 ||
      Name.back() == '.' || Name.back() == ' ' ||
      !llvm::json::isUTF8(llvm::StringRef(Name)))
    throw Error("unsafe_member_name");
  for (unsigned char C : Name)
    if (C < 32 || C == 127 ||
        std::string_view("/\\:*?\"<>|").find(C) != std::string_view::npos)
      throw Error("unsafe_member_name");
  auto Stem = Name.substr(0, Name.find('.'));
  std::string Upper(Stem);
  for (auto &C : Upper)
    if (C >= 'a' && C <= 'z')
      C -= 'a' - 'A';
  if (Upper == "CON" || Upper == "PRN" || Upper == "AUX" || Upper == "NUL" ||
      (Upper.size() == 4 &&
       (Upper.starts_with("COM") || Upper.starts_with("LPT")) &&
       Upper[3] >= '1' && Upper[3] <= '9'))
    throw Error("unsafe_member_name");
}

std::string nameKey(std::string_view Name) {
  std::string Key;
  const auto *Cursor = reinterpret_cast<const llvm::UTF8 *>(Name.data());
  const auto *End = Cursor + Name.size();
  while (Cursor != End) {
    llvm::UTF32 Code;
    if (llvm::convertUTF8Sequence(&Cursor, End, &Code,
                                  llvm::strictConversion) != llvm::conversionOK)
      throw Error("unsafe_member_name");
    Code = llvm::sys::unicode::foldCharSimple(Code);
    // Fixed-width scalar encoding is a comparison key, never a host path.
    for (unsigned I = 0; I != 4; ++I)
      Key.push_back(char(Code >> (I * 8)));
  }
  return Key;
}

#ifndef _WIN32
namespace {
struct FD {
  int Value;
  explicit FD(int Value) : Value(Value) {
    if (Value < 0)
      throw Error("input_unavailable");
  }
  ~FD() { close(Value); }
  FD(const FD &) = delete;
};

bool sameFile(const struct stat &A, const struct stat &B) {
#ifdef __APPLE__
  const bool Times = A.st_mtimespec.tv_sec == B.st_mtimespec.tv_sec &&
                     A.st_mtimespec.tv_nsec == B.st_mtimespec.tv_nsec &&
                     A.st_ctimespec.tv_sec == B.st_ctimespec.tv_sec &&
                     A.st_ctimespec.tv_nsec == B.st_ctimespec.tv_nsec;
#else
  const bool Times = A.st_mtim.tv_sec == B.st_mtim.tv_sec &&
                     A.st_mtim.tv_nsec == B.st_mtim.tv_nsec &&
                     A.st_ctim.tv_sec == B.st_ctim.tv_sec &&
                     A.st_ctim.tv_nsec == B.st_ctim.tv_nsec;
#endif
  return A.st_dev == B.st_dev && A.st_ino == B.st_ino &&
         A.st_mode == B.st_mode && A.st_size == B.st_size && Times;
}

struct Reader {
  const Limits &Budget;
  Snapshot Result;
  BlobStore Store;

  explicit Reader(const Limits &Budget)
      : Budget(Budget), Store(Budget.MaxInputBytes) {}

  void visit(int Descriptor, std::string Path, uint64_t Depth) {
    if (Depth > Budget.MaxDepth || Result.Artifacts.size() >= Budget.MaxEntries)
      throw Error("budget_exceeded");
    struct stat Before{}, After{};
    if (fstat(Descriptor, &Before) != 0)
      throw Error("input_unavailable");
    const bool Directory = S_ISDIR(Before.st_mode);
    if (!Directory && !S_ISREG(Before.st_mode))
      throw Error("unsupported_file_type");
    if (!Directory && Before.st_nlink != 1)
      throw Error("hard_link_not_admitted");
    Artifact Item;
    Item.MemberPath = Path;
    Item.Directory = Directory;
    Item.Kind = Directory ? "directory" : "opaque";
    if (!Directory) {
      if (Before.st_size < 0 ||
          uint64_t(Before.st_size) > Budget.MaxMemberBytes ||
          uint64_t(Before.st_size) > Budget.MaxInputBytes - Result.InputBytes)
        throw Error("budget_exceeded");
      auto Captured = Store.capture(Descriptor, uint64_t(Before.st_size));
      Item.Content = std::move(Captured.Content);
      Item.BlobHash = std::move(Captured.Hash);
      Result.InputBytes += Item.Content.size();
    }
    Result.Artifacts.push_back(std::move(Item));
    if (Directory) {
      const int Copy = dup(Descriptor);
      if (Copy < 0)
        throw Error("input_unavailable");
      DIR *Raw = fdopendir(Copy);
      if (!Raw) {
        close(Copy);
        throw Error("input_unavailable");
      }
      std::unique_ptr<DIR, int (*)(DIR *)> DirectoryStream(Raw, closedir);
      std::vector<std::string> Names;
      std::set<std::string> Keys;
      while (true) {
        errno = 0;
        auto *Entry = readdir(Raw);
        if (!Entry) {
          if (errno)
            throw Error("input_unavailable");
          break;
        }
        const std::string Name(Entry->d_name);
        if (Name == "." || Name == "..")
          continue;
        validateMemberName(Name);
        if (!Keys.insert(nameKey(Name)).second)
          throw Error("member_name_collision");
        if (Names.size() >= Budget.MaxEntries - Result.Artifacts.size())
          throw Error("budget_exceeded");
        Names.push_back(Name);
      }
      std::sort(Names.begin(), Names.end());
      for (const auto &Name : Names) {
        struct stat Child{};
        if (fstatat(Descriptor, Name.c_str(), &Child, AT_SYMLINK_NOFOLLOW) != 0)
          throw Error("input_changed");
        if (!S_ISDIR(Child.st_mode) && !S_ISREG(Child.st_mode))
          throw Error("unsupported_file_type");
        // O_NONBLOCK prevents a concurrent replacement with a FIFO hanging
        // before fstat. O_NOFOLLOW and dirfd anchoring exclude link escapes.
        FD Input(openat(Descriptor, Name.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        struct stat Opened{};
        if (fstat(Input.Value, &Opened) != 0 || !sameFile(Child, Opened))
          throw Error("input_changed");
        visit(Input.Value, Path.empty() ? Name : Path + "/" + Name, Depth + 1);
      }
    }
    if (fstat(Descriptor, &After) != 0 || !sameFile(Before, After))
      throw Error("input_changed");
  }
};
} // namespace
#endif

Snapshot capture(std::string_view Path, const Limits &Budget) {
  if (!Budget.MaxInputBytes || Budget.MaxInputBytes > Limits::HardInputBytes ||
      !Budget.MaxMemberBytes ||
      Budget.MaxMemberBytes > Limits::HardMemberBytes || !Budget.MaxEntries ||
      Budget.MaxEntries > Limits::HardEntries || !Budget.MaxDepth ||
      Budget.MaxDepth > Limits::HardDepth)
    throw Error("invalid_limit");
  if (Path.empty() || Path.size() > 32768 || Path.find('\0') != Path.npos)
    throw Error("invalid_path");
#ifdef _WIN32
  // A Windows handle-relative reader is required before admitting directory
  // input. A portable path-based fallback would reintroduce reparse races.
  throw Error("input_reader_unavailable");
#else
  // A trailing slash makes open(..., O_NOFOLLOW) dereference a directory
  // symlink on POSIX. Reject a terminal dot component and strip separators
  // before opening the selected root itself with no-follow semantics.
  while (Path.size() > 1 && Path.back() == '/')
    Path.remove_suffix(1);
  if (Path.ends_with("/.") || Path.ends_with("/.."))
    throw Error("invalid_path");
  FD Input(open(std::string(Path).c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  Reader Read{Budget};
  Read.visit(Input.Value, "", 0);
  auto &Result = Read.Result;
  std::sort(Result.Artifacts.begin(), Result.Artifacts.end(),
            [](const Artifact &A, const Artifact &B) {
              return A.MemberPath < B.MemberPath;
            });
  std::vector<std::string> Records;
  for (const auto &A : Result.Artifacts)
    Records.push_back(identity("member", {A.MemberPath, A.Kind, A.BlobHash}));
  std::vector<std::string_view> Fields(Records.begin(), Records.end());
  Result.ID = identity("snapshot", Fields);
  for (auto &A : Result.Artifacts) {
    A.ID = identity("occurrence", {Result.ID, A.MemberPath, A.BlobHash});
    if (!A.MemberPath.empty()) {
      auto Parent =
          std::filesystem::path(A.MemberPath).parent_path().generic_string();
      A.ParentID = identity("occurrence", {Result.ID, Parent, ""});
    }
  }
  Read.Store.seal();
  return std::move(Result);
#endif
}

} // namespace neverd::web
