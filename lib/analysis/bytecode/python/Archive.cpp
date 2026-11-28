//===- Archive.cpp - PyInstaller component inventory ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate the complete table of contents before expanding any member. Each
/// offset is relative to its immediate container, including a compressed PYZ.
//===----------------------------------------------------------------------===//

#include "Artifacts.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <zlib.h>

using namespace llvm;
using namespace neverd::python;

bool neverd::python::safeMemberName(StringRef Name) {
  if (Name.empty() || Name.size() > 4096 || !json::isUTF8(Name) ||
      Name.contains(':') ||
      llvm::any_of(Name, [](unsigned char C) { return C < 32 || C == 127; }))
    return false;
  for (;;) {
    size_t Slash = Name.find_first_of("/\\");
    auto Part = Name.take_front(Slash);
    if (Part.empty() || Part.ends_with(".") || Part.ends_with(" "))
      return false;
    if (Slash == StringRef::npos)
      return true;
    Name = Name.drop_front(Slash + 1);
  }
}
namespace {
constexpr size_t CookieBytes = 88;
constexpr StringLiteral CookieMagic("MEI\x0c\x0b\x0a\x0b\x0e");

struct Member {
  StringRef Name;
  size_t At, Size;
  std::optional<size_t> Expanded;
  bool Compressed;
  char Type;
};
std::string expand(Job &J, StringRef Data, const Member &M) {
  if (M.Expanded && *M.Expanded > limits::kMemberBytes)
    throw Failure{"budget_exceeded", M.At,
                  "Archive member expansion limit exceeded"};
  z_stream Z{};
  if (inflateInit(&Z) != Z_OK)
    throw Failure{"io_error", M.At, "Cannot initialize zlib"};
  struct End {
    z_stream &Z;
    ~End() { inflateEnd(&Z); }
  } Cleanup{Z};
  Z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(Data.data()));
  Z.avail_in = Data.size();
  std::array<char, 32768> Block;
  std::string Result;
  for (;;) {
    J.check(M.At + Z.total_in);
    Z.next_out = reinterpret_cast<Bytef *>(Block.data());
    Z.avail_out = Block.size();
    int Status = inflate(&Z, Z_NO_FLUSH);
    size_t N = Block.size() - Z.avail_out;
    if (N > limits::kMemberBytes - Result.size())
      throw Failure{"budget_exceeded", M.At,
                    "Archive member expansion limit exceeded"};
    if (M.Expanded && Result.size() + N > *M.Expanded)
      malformed(M.At, "Expanded member exceeds declared size");
    J.charge(J.Expanded, N, limits::kExpandedBytes, M.At);
    Result.append(Block.data(), N);
    if (Status == Z_STREAM_END)
      break;
    if (Status != Z_OK)
      malformed(M.At, "Invalid or truncated zlib member");
  }
  if (Z.avail_in || (M.Expanded && Result.size() != *M.Expanded))
    malformed(M.At, "Compressed extent or expanded size mismatch");
  return Result;
}

void visit(Job &J, unsigned Id, StringRef Bytes, ArrayRef<Member> Members,
           unsigned Depth, unsigned Version) {
  SmallVector<const Member *> Sorted;
  StringSet<> Names;
  for (const Member &M : Members) {
    auto Key = M.Name.lower();
    std::replace(Key.begin(), Key.end(), char(92), char(47));
    if (!Names.insert(Key).second)
      malformed(M.At, "Duplicate archive member name");
    if (M.Size)
      Sorted.push_back(&M);
  }
  llvm::sort(Sorted,
             [](const Member *A, const Member *B) { return A->At < B->At; });
  size_t End = 0;
  for (auto M : Sorted) {
    if (M->At < End)
      malformed(M->At, "Overlapping archive members");
    End = M->At + M->Size;
  }
  (*J.Components[Id].getAsObject())["toc_validated"] = true;
  auto Origin = J.Components[Id].getAsObject()->getInteger("data_file_offset");
  for (const Member &M : Members) {
    J.check(M.At);
    StringRef Stored = Bytes.substr(M.At, M.Size);
    std::string Expanded;
    if (M.Compressed)
      Expanded = expand(J, Stored, M);
    StringRef Data = M.Compressed ? StringRef(Expanded) : Stored;
    unsigned Child = J.component(M.Name, Data, Id);
    auto &Row = *J.Components[Child].getAsObject();
    Row["stored_offset"] = M.At;
    Row["stored_bytes"] = Stored.size();
    Row["stored_sha256"] =
        M.Compressed ? hash(Stored) : Row.getString("sha256")->str();
    Row["compression"] = M.Compressed ? "zlib" : "none";
    Row["member_type"] = std::string(1, M.Type);
    if (Origin) {
      Row["stored_file_offset"] = *Origin + M.At;
      if (!M.Compressed)
        Row["data_file_offset"] = *Origin + M.At;
    }
    try {
      if (M.Type == 'd') {
        Row["kind"] = "external_dependency";
        Row["dependency_package"] = M.Name.split(':').first.str();
        Row["dependency"] = M.Name.split(':').second.str();
        Row["status"] = "missing_member";
        Row.getArray("diagnostics")
            ->emplace_back(json::Object{
                {"message",
                 "External bundle member requires an accompanying package"},
                {"offset", M.At}});
        continue;
      }
      if (M.Type == 'n')
        unsupported(M.At, "Archive symbolic link is not followed");
      if (M.Type == 'o' || M.Type == 'N') {
        Row["kind"] = M.Type == 'o' ? "runtime_option" : "namespace";
        continue;
      }
      J.inspect(Child, Data, M.Name, Depth + 1, Version,
                M.Type == 's' || M.Type == 'm' || M.Type == 'M');
      if (M.Type == 's' && (!J.Selection || J.Selection == Child))
        if (auto Entry = J.Components[Child].getAsObject()->getInteger("root"))
          J.Root = *Entry;
    } catch (const Failure &F) {
      if (F.Status == "cancelled" || F.Status == "budget_exceeded")
        throw;
      J.diagnose(Child, F);
    }
  }
}
} // namespace

size_t neverd::python::findArchiveCookie(StringRef Bytes) {
  if (Bytes.size() < CookieMagic.size())
    return StringRef::npos;
  // A normal footer is in the last block. Skip blocks without the first
  // signature byte. For the rest, compare complete signatures from the
  // right, even when a signature crosses the block boundary.
  size_t End = Bytes.size() - CookieMagic.size() + 1;
  while (End) {
    size_t Begin = End - std::min(End, CookieBytes);
    size_t First = Bytes.slice(Begin, End).find(CookieMagic.front());
    if (First != StringRef::npos) {
      for (size_t Pos = End; Pos > Begin + First; --Pos)
        if (std::memcmp(Bytes.data() + Pos - 1, CookieMagic.data(),
                        CookieMagic.size()) == 0)
          return Pos - 1;
    }
    End = Begin;
  }
  return StringRef::npos;
}

void neverd::python::archive(Job &J, unsigned Id, StringRef B, size_t Cookie,
                             unsigned Depth) {
  if (B.size() - Cookie < CookieBytes)
    malformed(Cookie, "Truncated PyInstaller cookie");
  auto U32 = [&](size_t At) {
    return support::endian::read32be(B.data() + At);
  };
  uint64_t Size = U32(Cookie + 8), Toc = U32(Cookie + 12),
           Length = U32(Cookie + 16);
  unsigned Version = U32(Cookie + 20);
  size_t End = Cookie + CookieBytes;
  if (Size < CookieBytes || Size > End)
    malformed(Cookie, "Invalid CArchive package extent");
  size_t Start = End - Size;
  if (Toc + Length != Cookie - Start)
    malformed(Cookie + 12, "CArchive table does not end at the cookie");
  auto Library = B.substr(Cookie + 24, 64).split('\0').first;
  if (!safeMemberName(Library))
    malformed(Cookie + 24, "Invalid Python library name in cookie");
  auto &R = *J.Components[Id].getAsObject();
  R["kind"] = "pyinstaller_carchive";
  R["backend"] = "package";
  R["python_version_tag"] = Version;
  R["runtime_library"] = Library.str();
  R["package_offset"] = Start;
  R["cookie_offset"] = Cookie;
  std::vector<Member> Members;
  for (size_t P = Start + Toc; P < Cookie;) {
    J.check(P);
    if (Cookie - P < 18)
      malformed(P, "Truncated CArchive member record");
    size_t Record = U32(P), At = U32(P + 4), N = U32(P + 8),
           Unpacked = U32(P + 12);
    if (Record <= 18 || Record > Cookie - P || At > Toc || N > Toc - At)
      malformed(P, "Invalid CArchive member extent");
    unsigned Flag = uint8_t(B[P + 16]);
    char Type = B[P + 17];
    auto Field = B.substr(P + 18, Record - 18);
    auto [Name, Padding] = Field.split('\0');
    if (Name.size() == Field.size() ||
        llvm::any_of(Padding, [](char C) { return C != 0; }))
      malformed(P + 18, "Invalid CArchive name terminator");
    auto [Prefix, Leaf] = Name.split(':');
    if (Type == 'd' ? !safeMemberName(Prefix) || !safeMemberName(Leaf)
                    : !safeMemberName(Name))
      malformed(P + 18, "Unsafe archive member name");
    if (Flag > 1 || (!Flag && N != Unpacked))
      malformed(P + 8, "Invalid archive compression or size");
    if (Members.size() >= limits::kMembers)
      throw Failure{"budget_exceeded", P, "Archive member limit exceeded"};
    Members.push_back({Name, Start + At, N, Unpacked, bool(Flag), Type});
    P += Record;
  }
  // Older package writers encoded single-digit minor versions as 27 or 39.
  unsigned Runtime = Version >= 20 && Version < 40
                         ? Version / 10 * 100 + Version % 10
                         : Version;
  visit(J, Id, B, Members, Depth, Runtime);
}

void neverd::python::pyz(Job &J, unsigned Id, StringRef B, unsigned Depth) {
  if (B.size() < 12)
    malformed(0, "Truncated PYZ header");
  unsigned Magic = support::endian::read16le(B.data() + 4);
  if (B.substr(6, 2) != "\r\n")
    malformed(4, "Invalid Python magic in PYZ");
  auto &R = *J.Components[Id].getAsObject();
  R["kind"] = "pyinstaller_pyz";
  R["backend"] = "package";
  R["magic"] = Magic;
  unsigned Version = pythonVersion(support::endian::read32le(B.data() + 4),
                                   profileVersion(J.Profile));
  if (!Version)
    unsupported(4, "Unqualified PYZ marshal layout");
  size_t Toc = support::endian::read32be(B.data() + 8);
  if (Toc < 12 || Toc >= B.size())
    malformed(8, "Invalid PYZ table offset");
  Marshal Reader(J, B, Toc, Version);
  const auto &T = Reader.read();
  if (T.Type != Object::List && T.Type != Object::Dict)
    malformed(Toc, "Invalid PYZ table type");
  std::vector<Member> Members;
  auto Entry = [&](const Object &Name, const Object &Fields) {
    if ((Name.Type != Object::Unicode &&
         !(Version < 300 && Name.Type == Object::Bytes)) ||
        !safeMemberName(Name.Data))
      malformed(Name.Offset, "Unsafe PYZ module name");
    if (Fields.Type != Object::Tuple || Fields.Items.size() != 3 ||
        !llvm::all_of(Fields.Items, [](const Object *O) {
          return O->Type == Object::Int || O->Type == Object::False ||
                 O->Type == Object::True;
        }))
      malformed(Fields.Offset, "Invalid PYZ member fields");
    auto Tag =
        Fields.Items[0]->Type == Object::True ? 1 : Fields.Items[0]->Number;
    auto At = Fields.Items[1]->Number, N = Fields.Items[2]->Number;
    if (Tag < 0 || Tag > 3 || At < 0 || N < 0 || uint64_t(At) > Toc ||
        uint64_t(N) > Toc - At || (N && At < 12) || (Tag == 3 && N))
      malformed(Fields.Offset, "Invalid PYZ member extent");
    if (Members.size() >= limits::kMembers)
      throw Failure{"budget_exceeded", Fields.Offset,
                    "PYZ member limit exceeded"};
    Members.push_back({Name.Data,
                       size_t(At),
                       size_t(N),
                       {},
                       Tag != 3,
                       Tag == 3   ? 'N'
                       : Tag == 2 ? 'x'
                                  : 'm'});
  };
  if (T.Type == Object::Dict) {
    for (size_t I = 0; I < T.Items.size(); I += 2)
      Entry(*T.Items[I], *T.Items[I + 1]);
  } else {
    for (auto P : T.Items) {
      if (P->Type != Object::Tuple || P->Items.size() != 2)
        malformed(P->Offset, "Invalid PYZ member pair");
      Entry(*P->Items[0], *P->Items[1]);
    }
  }
  visit(J, Id, B, Members, Depth, Version);
}
