//===- Recovery.cpp - Python artifact dispatch and source extraction ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Routes immutable components, retaining original extents, hashes and a
/// distinct native backend for compiled images.
//===----------------------------------------------------------------------===//

#include "Artifacts.h"

#include "neverd/support/SHA256.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Base64.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <fstream>
#include <map>

using namespace llvm;
using namespace neverd::python;
namespace fs = std::filesystem;

void neverd::python::malformed(size_t Offset, StringRef M) {
  throw Failure{"malformed", Offset, M.str()};
}
void neverd::python::unsupported(size_t Offset, StringRef M) {
  throw Failure{"unsupported", Offset, M.str()};
}
void Job::check(size_t At) const {
  if (Stop())
    throw Failure{"cancelled", At, "Python recovery cancelled"};
}
void Job::charge(size_t &Used, size_t N, size_t Max, size_t At) {
  check(At);
  if (N > Max - Used)
    throw Failure{"budget_exceeded", At,
                  "Python recovery resource budget exceeded"};
  Used += N;
}
std::string neverd::python::hash(StringRef Bytes) {
  return toHex(sha256(arrayRefFromStringRef(Bytes)), true);
}
unsigned Job::component(StringRef Name, StringRef Bytes,
                        std::optional<unsigned> Parent) {
  check(0);
  if (!json::isUTF8(Name))
    malformed(0, "Invalid UTF-8 artifact name");
  if (Components.size() >= limits::kMembers)
    throw Failure{"budget_exceeded", 0, "Component count limit exceeded"};
  unsigned Id = Components.size();
  json::Object R{{"id", Id},
                 {"name", Name.str()},
                 {"bytes", Bytes.size()},
                 {"sha256", hash(Bytes)},
                 {"status", "metadata_only"},
                 {"kind", "data"},
                 {"backend", "inventory"},
                 {"diagnostics", json::Array{}}};
  if (Parent)
    R["parent"] = *Parent;
  Components.emplace_back(std::move(R));
  return Id;
}
void Job::diagnose(unsigned Id, const Failure &F) {
  auto &R = *Components[Id].getAsObject();
  R["status"] = F.Status;
  json::Object D{
      {"component", Id}, {"offset", F.Offset}, {"message", F.Message}};
  R.getArray("diagnostics")->emplace_back(json::Object(D));
  Diagnostics.emplace_back(std::move(D));
}
namespace {
fs::path filePath(StringRef S) {
  return fs::path(std::u8string(S.begin(), S.end()));
}
std::string portable(const fs::path &P) {
  auto U = P.generic_u8string();
  return {U.begin(), U.end()};
}
unsigned profileVersion(StringRef Profile) {
  return Profile == "cpython-3.10" ? 310 : Profile == "cpython-3.11" ? 311 : 0;
}
void checkProfile(StringRef Profile, unsigned Version, size_t At) {
  if (auto Requested = profileVersion(Profile);
      Requested && Requested != Version)
    unsupported(At, "Python profile conflicts with artifact version");
}
std::string readFile(const fs::path &P, size_t &Total, Job &J) {
  std::error_code EC;
  auto Size = fs::file_size(P, EC);
  if (EC || !fs::is_regular_file(P, EC))
    throw Failure{"io_error", 0, "Cannot read a regular artifact file"};
  J.charge(Total, Size, limits::kInputBytes, 0);
  std::ifstream In(P, std::ios::binary);
  std::string Bytes(Size, '\0');
  for (size_t At = 0; At < Size;) {
    J.check(At);
    size_t N = std::min<size_t>(Size - At, 256 * 1024);
    if (!In.read(Bytes.data() + At, N))
      throw Failure{"io_error", At, "Cannot read artifact bytes"};
    At += N;
  }
  if (In.peek() != std::char_traits<char>::eof() || In.bad())
    throw Failure{"io_error", 0, "Cannot read a stable file extent"};
  return Bytes;
}
bool nativeMagic(file_magic M) {
  return M == file_magic::elf_executable ||
         M == file_magic::elf_shared_object ||
         M == file_magic::elf_relocatable || M == file_magic::macho_object ||
         M == file_magic::macho_executable || M == file_magic::macho_bundle ||
         M == file_magic::macho_dynamically_linked_shared_lib ||
         M == file_magic::pecoff_executable ||
         M == file_magic::macho_universal_binary;
}
void native(Job &J, unsigned Id, StringRef Bytes, StringRef Name) {
  auto &R = *J.Components[Id].getAsObject();
  R["kind"] = "native_image";
  bool Cython = false, Nuitka = false, Extension = false;
  auto classifyName = [&](StringRef Name) {
    J.charge(J.Objects, 1, limits::kObjects, 0);
    auto N = Name.ltrim('_');
    Extension |= N.starts_with("PyInit_");
    Cython |= N.starts_with("pyx_");
    Nuitka |= N.starts_with("Nuitka_") || N.starts_with("NuitkaCompiled");
  };
  auto inspectSymbols = [&](const object::ObjectFile &Obj) {
    auto inspectRange = [&](auto Symbols) {
      for (const auto &Symbol : Symbols) {
        auto N = Symbol.getName();
        if (!N) {
          consumeError(N.takeError());
          malformed(0, "Invalid native symbol name");
        }
        classifyName(*N);
      }
    };
    inspectRange(Obj.symbols());
    if (const auto *ELF = dyn_cast<object::ELFObjectFileBase>(&Obj))
      inspectRange(ELF->getDynamicSymbolIterators());
    if (const auto *PE = dyn_cast<object::COFFObjectFile>(&Obj))
      for (const auto &Export : PE->export_directories()) {
        StringRef N;
        if (auto E = Export.getSymbolName(N)) {
          consumeError(std::move(E));
          malformed(0, "Invalid native export name");
        }
        classifyName(N);
      }
  };
  MemoryBufferRef Buffer(Bytes, Name);
  if (identify_magic(Bytes) == file_magic::macho_universal_binary) {
    auto Fat = object::MachOUniversalBinary::create(Buffer);
    if (!Fat) {
      consumeError(Fat.takeError());
      malformed(0, "Invalid universal native header");
    }
    for (const auto &Arch : (*Fat)->objects()) {
      auto Slice = Arch.getAsObjectFile();
      if (!Slice) {
        consumeError(Slice.takeError());
        malformed(Arch.getOffset(), "Invalid universal native slice");
      }
      inspectSymbols(**Slice);
    }
    R["native_format"] = "Mach-O universal";
  } else {
    auto Obj = object::ObjectFile::createObjectFile(Buffer);
    if (!Obj) {
      consumeError(Obj.takeError());
      malformed(0, "Invalid native object header");
    }
    R["native_format"] = (*Obj)->getFileFormatName().str();
    inspectSymbols(**Obj);
  }
  if (Cython && !Nuitka)
    R["producer_evidence"] = "Cython symbol family";
  if (Nuitka && !Cython)
    R["producer_evidence"] = "Nuitka symbol family";
  if (Extension)
    R["kind"] = "native_extension";
  R["backend"] = "native";
  if (J.NativeSelection == Id)
    J.NativeBytes = Bytes.str();
}
StringRef encodingCookie(StringRef Bytes) {
  Bytes.consume_front("\xef\xbb\xbf");
  for (unsigned I = 0; I < 2; ++I) {
    auto [Line, Rest] = Bytes.split('\n');
    Bytes = Rest;
    Line = Line.ltrim(" \t\f");
    if (!Line.starts_with("#")) {
      if (Line.trim().empty())
        continue;
      break;
    }
    size_t P = Line.find("coding");
    if (P == StringRef::npos)
      continue;
    auto Tail = Line.drop_front(P + 6);
    if (!Tail.consume_front(":") && !Tail.consume_front("="))
      continue;
    Tail = Tail.ltrim(" \t");
    return Tail.take_while(
        [](char C) { return isAlnum(C) || C == '-' || C == '_' || C == '.'; });
  }
  return {};
}
void source(Job &J, unsigned Id, StringRef Bytes) {
  auto &C = *J.Components[Id].getAsObject();
  C["kind"] = "python_source";
  C["backend"] = "python";
  if (Bytes.contains('\0'))
    malformed(0, "NUL byte in Python source");
  J.charge(J.Text, Bytes.size(), limits::kTextBytes, 0);
  auto Enc = encodingCookie(Bytes).lower();
  C["encoding"] = Enc.empty() ? "utf-8" : Enc;
  json::Object F{{"id", J.Functions.size()},
                 {"component", Id},
                 {"name", "<module>"},
                 {"offset", 0},
                 {"end_offset", Bytes.size()},
                 {"status", "extracted_source"},
                 {"instructions", json::Array{}},
                 {"diagnostics", json::Array{}}};
  bool Latin = Enc == "latin-1" || Enc == "latin1" || Enc == "iso-8859-1" ||
               Enc == "iso8859-1";
  if ((Enc.empty() || Enc == "utf-8" || Enc == "utf8" || Enc == "utf_8" ||
       Enc == "ascii" || Enc == "us-ascii") &&
      json::isUTF8(Bytes)) {
    if ((Enc == "ascii" || Enc == "us-ascii") &&
        llvm::any_of(Bytes, [](unsigned char B) { return B >= 128; }))
      malformed(0, "Non-ASCII byte in explicitly ASCII source");
    F["source"] = Bytes.str();
  } else {
    F["source_base64"] = encodeBase64(Bytes);
    if (Latin) {
      std::string Text;
      for (unsigned char B : Bytes) {
        if (B < 128)
          Text += char(B);
        else {
          Text += char(192 | (B >> 6));
          Text += char(128 | (B & 63));
        }
      }
      F["source"] = std::move(Text);
    } else {
      F.getArray("diagnostics")
          ->emplace_back(json::Object{
              {"message", "Source encoding is unavailable for display; "
                          "original bytes can be exported"}});
    }
  }
  if (!J.Root)
    J.Root = J.Functions.size();
  C["root"] = J.Functions.size();
  C["status"] = "extracted_source";
  J.Functions.emplace_back(std::move(F));
}
bool selected(const Job &J, unsigned Id) {
  if (J.NativeSelection)
    return false;
  if (J.Selection)
    return J.Selection == Id;
  const auto &R = *J.Components[Id].getAsObject();
  auto Parent = R.getInteger("parent");
  if (!Parent)
    return true;
  auto Type = R.getString("member_type");
  if (Type == "m" || Type == "M")
    return !J.Root &&
           J.Components[*Parent].getAsObject()->getString("kind") ==
               "pyinstaller_pyz" &&
           *Parent == 0;
  return true;
}
} // namespace
void Job::inspect(unsigned Id, StringRef B, StringRef Name, unsigned Depth,
                  unsigned Version, bool RawCode) {
  check(0);
  if (Depth > limits::kDepth)
    throw Failure{"budget_exceeded", 0, "Archive nesting limit exceeded"};
  auto &R = *Components[Id].getAsObject();
  StringRef Ext = sys::path::extension(Name);
  if (B.starts_with(StringRef("PYZ\0", 4))) {
    if (B.size() >= 8) {
      unsigned Tag = pythonVersion(support::endian::read16le(B.data() + 4));
      if (Version && Tag && Tag != Version)
        malformed(4, "PYZ magic conflicts with enclosing runtime");
      checkProfile(Profile, Tag, 4);
    }
    pyz(*this, Id, B, Depth);
    return;
  }
  if (RawCode) {
    R["kind"] = "marshalled_code";
    R["backend"] = "python";
    R["python_version_tag"] = Version;
    if (Version != 310 && Version != 311)
      unsupported(0, "Unqualified packaging runtime for raw code");
    checkProfile(Profile, Version, 0);
    if (!selected(*this, Id)) {
      R["status"] = "available";
      return;
    }
    recoverCode(*this, Id, B, 0, Version);
    return;
  }
  if (Ext.equals_insensitive(".pyc") ||
      (B.size() >= 4 && B.substr(2, 2) == "\r\n" &&
       pythonVersion(support::endian::read16le(B.data())) &&
       !Ext.equals_insensitive(".py"))) {
    R["kind"] = "pyc";
    R["backend"] = "python";
    if (B.size() < 4 || B.substr(2, 2) != "\r\n")
      malformed(0, "Invalid or truncated pyc magic");
    unsigned Magic = support::endian::read16le(B.data());
    R["magic"] = Magic;
    unsigned Tag = pythonVersion(Magic);
    if (!Tag)
      unsupported(0, "Unqualified CPython magic and code layout");
    checkProfile(Profile, Tag, 0);
    if (B.size() < 16)
      malformed(B.size(), "Truncated pyc header");
    unsigned Flags = support::endian::read32le(B.data() + 4);
    if (Flags != 0 && Flags != 1 && Flags != 3)
      malformed(4, "Invalid pyc invalidation flags");
    R["header_mode"] = Flags == 0   ? "timestamp"
                       : Flags == 1 ? "unchecked_hash"
                                    : "checked_hash";
    if (Flags)
      R["source_hash"] = toHex(B.substr(8, 8), true);
    else {
      R["timestamp"] = support::endian::read32le(B.data() + 8);
      R["source_size"] = support::endian::read32le(B.data() + 12);
    }
    R["optimization"] = "unknown";
    R["dialect"] = Tag == 310 ? "CPython 3.10" : "CPython 3.11";
    if (!selected(*this, Id)) {
      R["status"] = "available";
      return;
    }
    recoverCode(*this, Id, B, 16, Tag);
    return;
  }
  if (Ext.equals_insensitive(".py")) {
    if (selected(*this, Id))
      source(*this, Id, B);
    else {
      R["kind"] = "python_source";
      R["backend"] = "python";
      R["status"] = "available";
    }
    return;
  }
  auto Cookie = B.rfind(StringRef(CookieMagic, sizeof(CookieMagic)));
  if (Cookie != StringRef::npos) {
    archive(*this, Id, B, Cookie, Depth);
    return;
  }
  if (nativeMagic(identify_magic(B))) {
    native(*this, Id, B, Name);
    return;
  }
  if (Ext.equals_insensitive(".marshal") && profileVersion(Profile)) {
    R["kind"] = "marshalled_code";
    R["backend"] = "python";
    if (selected(*this, Id))
      recoverCode(*this, Id, B, 0, profileVersion(Profile));
    return;
  }
  if (Id == 0 || Ext.equals_insensitive(".pyz") ||
      Ext.equals_insensitive(".pkg") || Ext.equals_insensitive(".rpyc"))
    unsupported(0, "Unrecognized Python artifact or packaging signature");
}

namespace {
void directory(Job &J, const fs::path &Base) {
  unsigned Root = J.component(portable(Base.filename()), "", {});
  auto &D = *J.Components[Root].getAsObject();
  D["kind"] = "directory";
  D["backend"] = "package";
  D.erase("sha256");
  D.erase("bytes");
  std::vector<std::string> Paths, Links;
  size_t Entries = 0;
  std::error_code EC;
  for (fs::recursive_directory_iterator It(Base, EC), End; !EC && It != End;
       It.increment(EC)) {
    J.check(0);
    if (It.depth() > int(limits::kDepth) || ++Entries >= limits::kMembers)
      throw Failure{"budget_exceeded", 0,
                    "Directory traversal budget exceeded"};
    auto Status = It->symlink_status(EC);
    if (EC)
      break;
    std::string Name = portable(It->path().lexically_relative(Base));
    if (!safeMemberName(Name))
      malformed(0, "Unsafe directory member name");
    if (fs::is_symlink(Status)) {
      Links.push_back(std::move(Name));
      continue;
    }
    if (fs::is_regular_file(Status))
      Paths.push_back(std::move(Name));
  }
  if (EC)
    throw Failure{"io_error", 0, "Cannot enumerate bundle directory"};
  llvm::sort(Paths);
  llvm::sort(Links);
  size_t Total = 0;
  std::string Identity;
  std::map<std::string, unsigned> Files;
  bool HasPackage = false;
  for (const auto &Name : Links) {
    auto Id = J.component(Name, "", Root);
    auto &R = *J.Components[Id].getAsObject();
    R["kind"] = "symbolic_link";
    R.erase("sha256");
    R.erase("bytes");
    R.getArray("diagnostics")
        ->emplace_back(
            json::Object{{"message", "Filesystem link was not followed"}});
    Identity += Name;
    Identity += '\0';
    Identity += "symlink";
  }
  for (const auto &Name : Paths) {
    auto Data = readFile(Base / filePath(Name), Total, J);
    unsigned Id = J.component(Name, Data, Root);
    Files.emplace(Name, Id);
    auto &Row = *J.Components[Id].getAsObject();
    Identity += Name;
    Identity += '\0';
    Identity += *Row.getString("sha256");
    Row["data_file_offset"] = 0;
    Row["file"] = Name;
    try {
      J.inspect(Id, Data, Name, 1);
    } catch (const Failure &F) {
      if (F.Status == "cancelled" || F.Status == "budget_exceeded")
        throw;
      J.diagnose(Id, F);
    }
    HasPackage |=
        J.Components[Id].getAsObject()->getString("kind") ==
            "pyinstaller_carchive" &&
        J.Components[Id].getAsObject()->getBoolean("toc_validated") == true;
  }
  auto &Result = *J.Components[Root].getAsObject();
  Result["sha256"] = hash(Identity);
  Result["bytes"] = Total;
  if (!HasPackage)
    unsupported(0, "Directory contains no validated PyInstaller package");
  Result["kind"] = "pyinstaller_onedir";
  auto memberPath = [](StringRef Path) {
    std::string Name = Path.str();
    std::replace(Name.begin(), Name.end(), char(92), char(47));
    return Name;
  };
  std::map<std::pair<unsigned, std::string>, unsigned> StoredMembers;
  for (const auto &Entry : J.Components) {
    const auto &R = *Entry.getAsObject();
    auto Type = R.getString("member_type");
    auto Parent = R.getInteger("parent");
    if (Parent && Type && *Type != "d" && *Type != "n" && *Type != "o" &&
        *Type != "N")
      StoredMembers.emplace(
          std::make_pair(*Parent, memberPath(*R.getString("name"))),
          *R.getInteger("id"));
  }
  for (auto &Entry : J.Components) {
    auto &R = *Entry.getAsObject();
    auto Dep = R.getString("dependency");
    if (!Dep)
      continue;
    const auto &Container =
        *J.Components[*R.getInteger("parent")].getAsObject();
    auto Path = Container.getString("file");
    if (!Path)
      continue;
    auto Package = filePath(*Path).parent_path() /
                   filePath(memberPath(*R.getString("dependency_package")));
    auto Owner = Files.find(portable(Package));
    if (Owner == Files.end() ||
        J.Components[Owner->second].getAsObject()->getBoolean(
            "toc_validated") != true)
      continue;
    auto Relative = memberPath(*Dep);
    auto Member = StoredMembers.find({Owner->second, Relative});
    if (Member != StoredMembers.end())
      R["resolved_component"] = Member->second;
    else {
      auto Found =
          Files.find(portable(Package.parent_path() / filePath(Relative)));
      if (Found == Files.end())
        Found = Files.find(
            portable(Package.parent_path() / "_internal" / filePath(Relative)));
      if (Found != Files.end())
        R["resolved_component"] = Found->second;
    }
    if (R.getInteger("resolved_component")) {
      R["status"] = "metadata_only";
      R["diagnostics"] = json::Array{};
    }
  }
}
template <class Read> json::Object run(Job &J, Read ReadInput) {
  StringRef Status = "metadata_only";
  try {
    J.check(0);
    if (J.Profile != "auto" && !profileVersion(J.Profile) &&
        J.Profile != "renpy")
      unsupported(0, "Unknown Python recovery profile");
    ReadInput();
    if (J.Selection && *J.Selection >= J.Components.size())
      unsupported(0, "Unknown Python component");
    for (auto &V : J.Components) {
      auto S = *V.getAsObject()->getString("status");
      if (S == "malformed" || S == "partial_reconstruction" ||
          S == "unsupported" || S == "missing_member") {
        Status = V.getAsObject()->getInteger("id") == 0
                     ? S
                     : StringRef("partial_reconstruction");
        break;
      }
      if (S == "disassembly_only" && Status == "metadata_only")
        Status = S;
      if (S == "reconstructed_source" ||
          (S == "extracted_source" && Status == "metadata_only"))
        Status = S;
    }
    if (J.Selection)
      Status = *J.Components[*J.Selection].getAsObject()->getString("status");
  } catch (const Failure &F) {
    Status = F.Status;
    J.Diagnostics.emplace_back(
        json::Object{{"offset", F.Offset}, {"message", F.Message}});
  }
  json::Object R{{"schema", "language-recovery-v1"},
                 {"language", "Python"},
                 {"profile", (J.Profile == "auto" || J.Profile == "renpy" ||
                              profileVersion(J.Profile))
                                 ? J.Profile.str()
                                 : "unknown"},
                 {"status", Status},
                 {"root", nullptr},
                 {"evidence", json::Object{{"dialect", "Python"},
                                           {"bytecode_representation",
                                            "versioned on-disk wordcode"}}}};
  if (J.Root)
    R["root"] = *J.Root;
  if (!J.Components.empty()) {
    const auto &First = *J.Components.front().getAsObject();
    if (auto S = First.getString("sha256"))
      R["input_sha256"] = S->str();
    if (auto S = First.getInteger("bytes"))
      R["input_bytes"] = *S;
  }
  R["components"] = std::move(J.Components);
  R["functions"] = std::move(J.Functions);
  R["diagnostics"] = std::move(J.Diagnostics);
  return R;
}
void fromPath(Job &J, StringRef Path) {
  fs::path P = filePath(Path);
  std::error_code EC;
  if (fs::is_directory(P, EC)) {
    directory(J, P);
    return;
  }
  size_t Total = 0;
  auto Data = readFile(P, Total, J);
  auto Name = portable(P.filename());
  unsigned Id = J.component(Name, Data, {});
  (*J.Components[Id].getAsObject())["data_file_offset"] = 0;
  try {
    J.inspect(Id, Data, Name);
  } catch (const Failure &F) {
    J.diagnose(Id, F);
    if (F.Status == "cancelled" || F.Status == "budget_exceeded")
      throw;
  }
}
} // namespace
json::Object neverd::python::recover(StringRef Bytes, StringRef Name,
                                     StringRef Profile, Cancel Stop,
                                     int Component) {
  Job J{Profile, Stop};
  if (Component >= 0)
    J.Selection = Component;
  return run(J, [&] {
    if (Component < -1)
      unsupported(0, "Invalid component selection");
    if (Bytes.size() > limits::kInputBytes)
      throw Failure{"budget_exceeded", 0, "Python input byte limit exceeded"};
    unsigned Id = J.component(Name, Bytes, {});
    (*J.Components[Id].getAsObject())["data_file_offset"] = 0;
    try {
      J.inspect(Id, Bytes, Name);
    } catch (const Failure &F) {
      J.diagnose(Id, F);
      if (F.Status == "budget_exceeded" || F.Status == "cancelled")
        throw;
    }
  });
}
json::Object neverd::python::recoverFile(StringRef Path, StringRef Profile,
                                         Cancel Stop, int Component) {
  Job J{Profile, Stop};
  if (Component >= 0)
    J.Selection = Component;
  return run(J, [&] {
    if (Component < -1)
      unsupported(0, "Invalid component selection");
    fromPath(J, Path);
  });
}
json::Object neverd::python::exportNative(StringRef Path, StringRef Profile,
                                          unsigned Id, StringRef Expected,
                                          StringRef Destination, Cancel Stop) {
  Job J{Profile, Stop};
  J.NativeSelection = Id;
  auto Report = run(J, [&] { fromPath(J, Path); });
  if (Report.getString("status") == "cancelled" ||
      Report.getString("status") == "budget_exceeded")
    return Report;
  if (J.NativeBytes.empty() || hash(J.NativeBytes) != Expected)
    return json::Object{
        {"status", "error"},
        {"message", "Native component is unavailable or its identity changed"}};
  std::error_code EC;
  raw_fd_ostream Out(Destination, EC, sys::fs::CD_CreateNew, sys::fs::FA_Write,
                     sys::fs::OF_None);
  if (EC)
    return json::Object{{"status", "io_error"}, {"message", EC.message()}};
  StringRef Remaining = J.NativeBytes;
  while (!Remaining.empty() && !Out.has_error() && !Stop()) {
    StringRef Chunk = Remaining.take_front(256 * 1024);
    Out << Chunk;
    Remaining = Remaining.drop_front(Chunk.size());
  }
  Out.close();
  if (Out.has_error()) {
    auto Why = Out.error().message();
    Out.clear_error();
    sys::fs::remove(Destination);
    return json::Object{{"status", "io_error"}, {"message", Why}};
  }
  if (!Remaining.empty() || Stop()) {
    sys::fs::remove(Destination);
    return json::Object{{"status", "cancelled"},
                        {"message", "Native export cancelled"}};
  }
  return json::Object{{"status", "exported"}, {"sha256", Expected.str()}};
}
namespace {
// Preserve the small, buffered writes from the JSON formatter while bounding
// the backing string even when the full inventory cannot fit in a worker reply.
class ReportStream final : public raw_ostream {
  std::string &Text;
  bool Full = false;

  void write_impl(const char *Data, size_t Size) override {
    if (Full)
      return;
    if (Size > limits::kReportBytes - Text.size()) {
      Full = true;
      return;
    }
    Text.append(Data, Size);
  }
  uint64_t current_pos() const override { return Text.size(); }

public:
  explicit ReportStream(std::string &Text) : Text(Text) {}
  bool exceeded() const { return Full; }
};
} // namespace

std::string neverd::python::serializeReport(json::Object Report) {
  json::Value Value(std::move(Report));
  std::string Text;
  {
    ReportStream OS(Text);
    OS << Value;
    OS.flush();
    if (!OS.exceeded())
      return Text;
  }
  const auto &Full = *Value.getAsObject();
  json::Object Summary{
      {"schema", "language-recovery-v1"},
      {"language", "Python"},
      {"status", "budget_exceeded"},
      {"functions", json::Array{}},
      {"components", json::Array{}},
      {"root", nullptr},
      {"evidence", json::Object{{"dialect", "Python"}}},
      {"diagnostics",
       json::Array{
           json::Object{{"message", "Python report exceeds export budget"}}}}};
  if (auto Profile = Full.getString("profile"))
    if (Profile->size() <= 64)
      Summary["profile"] = Profile->str();
  if (auto Hash = Full.getString("input_sha256"))
    if (Hash->size() == 64)
      Summary["input_sha256"] = Hash->str();
  if (auto Size = Full.getInteger("input_bytes"))
    Summary["input_bytes"] = *Size;
  Text.clear();
  raw_string_ostream OS(Text);
  OS << json::Value(std::move(Summary));
  OS.flush();
  return Text;
}
