//===- COFFARMFormatTests.cpp - Windows ARM PE format tests --------------===//

#include "gtest/gtest.h"

#include "NeverDLiftFixture.h"
#include "neverd/Support/BinaryEncoding.h"
#include "neverd/Support/BinaryLoading.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/FunctionDiscovery.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace neverd;
namespace fs = std::filesystem;

fs::path fixture(llvm::StringRef Name) {
  return fs::path(TEST_OBJ_DIR) / Name.str();
}

class COFFARMFormat : public ::testing::Test {
protected:
  void SetUp() override {
    const auto *Info = ::testing::UnitTest::GetInstance()->current_test_info();
    TempDir = fs::temp_directory_path() /
              (std::string("neverd_coff_arm_") + Info->name());
    fs::remove_all(TempDir);
    fs::create_directories(TempDir);
  }

  void TearDown() override {
    if (!::testing::Test::HasFailure())
      fs::remove_all(TempDir);
  }

  fs::path writeMutation(llvm::StringRef Name,
                         const std::vector<uint8_t> &Bytes) const {
    fs::path Path = TempDir / Name.str();
    std::fstream Out(Path, std::ios::binary | std::ios::out | std::ios::trunc);
    Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    EXPECT_TRUE(Out.good());
    return Path;
  }

private:
  fs::path TempDir;
};

std::vector<uint8_t> readFile(const fs::path &Path) {
  std::ifstream In(Path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(In), {});
}

std::unique_ptr<llvm::object::COFFObjectFile>
createCOFFObject(const std::vector<uint8_t> &Bytes) {
  llvm::StringRef Data(reinterpret_cast<const char *>(Bytes.data()),
                       Bytes.size());
  auto ObjOrErr = llvm::object::COFFObjectFile::create(
      llvm::MemoryBufferRef(Data, "COFFARMFormat fixture"));
  if (!ObjOrErr) {
    ADD_FAILURE() << llvm::toString(ObjOrErr.takeError());
    return nullptr;
  }
  return std::move(*ObjOrErr);
}

size_t exceptionDirectoryFileOffset(const llvm::object::COFFObjectFile &Obj) {
  const llvm::object::data_directory *Dir =
      Obj.getDataDirectory(llvm::COFF::EXCEPTION_TABLE);
  EXPECT_NE(Dir, nullptr);
  if (!Dir)
    return 0;
  const char *FileBegin = Obj.getData().data();
  const char *DirPtr = reinterpret_cast<const char *>(Dir);
  EXPECT_GE(DirPtr, FileBegin);
  EXPECT_LE(DirPtr + sizeof(*Dir), FileBegin + Obj.getData().size());
  return static_cast<size_t>(DirPtr - FileBegin);
}

std::optional<size_t> rvaFileOffset(const llvm::object::COFFObjectFile &Obj,
                                    uint32_t RVA) {
  uintptr_t Ptr = 0;
  if (llvm::Error Err = Obj.getRvaPtr(RVA, Ptr)) {
    llvm::consumeError(std::move(Err));
    return std::nullopt;
  }
  uintptr_t FileBegin = reinterpret_cast<uintptr_t>(Obj.getData().data());
  uintptr_t FileEnd = FileBegin + Obj.getData().size();
  if (Ptr < FileBegin || Ptr > FileEnd)
    return std::nullopt;
  return static_cast<size_t>(Ptr - FileBegin);
}

std::optional<uint32_t> rawExportRVA(const llvm::object::COFFObjectFile &Obj,
                                     llvm::StringRef Name) {
  for (auto I = Obj.export_directory_begin(), E = Obj.export_directory_end();
       I != E; ++I) {
    llvm::StringRef ExportName;
    if (llvm::Error Err = I->getSymbolName(ExportName)) {
      llvm::consumeError(std::move(Err));
      continue;
    }
    if (ExportName != Name)
      continue;
    uint32_t RVA = 0;
    if (llvm::Error Err = I->getExportRVA(RVA)) {
      llvm::consumeError(std::move(Err));
      return std::nullopt;
    }
    return RVA;
  }
  return std::nullopt;
}

const Export *findExport(const BinaryImage &Img, llvm::StringRef Name) {
  auto It = std::find_if(Img.Exports.begin(), Img.Exports.end(),
                         [&](const Export &E) { return E.Name == Name; });
  return It == Img.Exports.end() ? nullptr : &*It;
}

std::optional<std::string> lowFunctionBody(llvm::StringRef Output,
                                           llvm::StringRef Name) {
  std::string Header = (llvm::Twine("func ") + Name + " @").str();
  size_t Begin = Output.find(Header);
  if (Begin == llvm::StringRef::npos)
    return std::nullopt;
  size_t End = Output.find("\nfunc ", Begin + Header.size());
  return Output.slice(Begin, End == llvm::StringRef::npos ? Output.size() : End)
      .str();
}

std::optional<std::string> cFunctionBody(llvm::StringRef Output,
                                         llvm::StringRef Name) {
  std::string Header = (llvm::Twine(Name) + "(").str();
  size_t SearchFrom = 0;
  while (true) {
    size_t Begin = Output.find(Header, SearchFrom);
    if (Begin == llvm::StringRef::npos)
      return std::nullopt;
    size_t OpenBrace = Output.find('{', Begin + Header.size());
    size_t Semicolon = Output.find(';', Begin + Header.size());
    if (OpenBrace != llvm::StringRef::npos &&
        (Semicolon == llvm::StringRef::npos || OpenBrace < Semicolon)) {
      unsigned Depth = 0;
      for (size_t I = OpenBrace; I < Output.size(); ++I) {
        if (Output[I] == '{')
          ++Depth;
        else if (Output[I] == '}' && --Depth == 0)
          return Output.slice(Begin, I + 1).str();
      }
      return std::nullopt;
    }
    SearchFrom = Begin + Header.size();
  }
}

void expectLeafSemantics(llvm::StringRef Body) {
  EXPECT_TRUE(Body.contains("LOAD"));
  EXPECT_TRUE(Body.contains("STORE"));
  EXPECT_TRUE(Body.contains("INT_ADD"));
  EXPECT_TRUE(Body.contains("RETURN"));
  EXPECT_TRUE(Body.contains("INT_MULT") ||
              (Body.contains("INT_LEFT") && Body.contains("INT_ADD")))
      << "expected multiply semantics in pe_leaf:\n"
      << Body.str();
}

void expectStackySemantics(llvm::StringRef Body) {
  for (llvm::StringRef Opcode : {"LOAD", "STORE", "INT_ADD", "CALL",
                                 "RETURN"})
    EXPECT_TRUE(Body.contains(Opcode))
        << "expected " << Opcode.str() << " in pe_stacky:\n" << Body.str();
}

void expectNoOddFunctionAddresses(llvm::StringRef Output) {
  while (!Output.empty()) {
    auto [Line, Rest] = Output.split('\n');
    Output = Rest;
    if (!Line.starts_with("func "))
      continue;
    size_t Marker = Line.find(" @ 0x");
    ASSERT_NE(Marker, llvm::StringRef::npos) << Line.str();
    llvm::StringRef Hex = Line.drop_front(Marker + 5);
    size_t HexEnd = Hex.find_first_of(" \t(");
    Hex = Hex.take_front(HexEnd);
    uint64_t Addr = 0;
    ASSERT_FALSE(Hex.getAsInteger(16, Addr)) << Line.str();
    EXPECT_EQ(Addr & 1u, 0u) << "odd-address function: " << Line.str();
  }
}

void expectLeafCallResultStored(llvm::StringRef Body) {
  size_t Call = Body.find("pe_leaf(");
  ASSERT_NE(Call, llvm::StringRef::npos) << Body.str();
  size_t StatementStart = Body.rfind(';', Call);
  size_t OpenBrace = Body.rfind('{', Call);
  if (StatementStart == llvm::StringRef::npos ||
      (OpenBrace != llvm::StringRef::npos && OpenBrace > StatementStart))
    StatementStart = OpenBrace;
  StatementStart =
      StatementStart == llvm::StringRef::npos ? 0 : StatementStart + 1;
  size_t Equals = Body.find('=', StatementStart);
  ASSERT_NE(Equals, llvm::StringRef::npos) << Body.str();
  ASSERT_LT(Equals, Call) << "pe_leaf call is not assigned in its statement:\n"
                          << Body.str();
  llvm::StringRef LHS = Body.slice(StatementStart, Equals);
  LHS = LHS.trim();
  ASSERT_FALSE(LHS.empty()) << Body.str();
  ASSERT_TRUE(std::all_of(
      LHS.begin(), LHS.end(),
      [](char C) {
        return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
      }))
      << "unexpected call-result expression: " << LHS.str();

  size_t CallEnd = Body.find(';', Call);
  ASSERT_NE(CallEnd, llvm::StringRef::npos) << Body.str();
  llvm::StringRef Rest = Body.drop_front(CallEnd + 1);
  bool Stored = false;
  while (!Rest.empty()) {
    auto [Line, Remaining] = Rest.split('\n');
    Rest = Remaining;
    Line = Line.trim();
    llvm::StringRef RHS;
    if (Line.starts_with("*(")) {
      size_t StoreEquals = Line.find('=');
      size_t Semicolon = Line.rfind(';');
      if (StoreEquals == llvm::StringRef::npos ||
          Semicolon == llvm::StringRef::npos || StoreEquals >= Semicolon)
        continue;
      RHS = Line.slice(StoreEquals + 1, Semicolon).trim();
    } else if (Line.starts_with("neverd_mem_store_")) {
      size_t Open = Line.find('(');
      if (Open == llvm::StringRef::npos)
        continue;
      unsigned Depth = 1;
      size_t Comma = llvm::StringRef::npos;
      size_t Close = llvm::StringRef::npos;
      for (size_t I = Open + 1; I < Line.size(); ++I) {
        if (Line[I] == '(')
          ++Depth;
        else if (Line[I] == ')') {
          if (--Depth == 0) {
            Close = I;
            break;
          }
        } else if (Line[I] == ',' && Depth == 1 &&
                   Comma == llvm::StringRef::npos) {
          Comma = I;
        }
      }
      if (Comma == llvm::StringRef::npos || Close == llvm::StringRef::npos)
        continue;
      RHS = Line.slice(Comma + 1, Close).trim();
    } else {
      continue;
    }
    while (RHS.starts_with("(")) {
      size_t Close = RHS.find(')');
      if (Close == llvm::StringRef::npos)
        break;
      llvm::StringRef Cast = RHS.slice(1, Close).trim();
      if (Cast.empty() || !std::all_of(Cast.begin(), Cast.end(), [](char C) {
            return std::isalnum(static_cast<unsigned char>(C)) || C == '_' ||
                   C == '*' || std::isspace(static_cast<unsigned char>(C));
          }))
        break;
      RHS = RHS.drop_front(Close + 1).trim();
    }
    if (RHS == LHS) {
      Stored = true;
      break;
    }
  }
  EXPECT_TRUE(Stored) << "pe_leaf result " << LHS.str()
                      << " is not stored after the call:\n"
                      << Body.str();
}

void expectLeafCallUsesParameter(llvm::StringRef Body,
                                 llvm::StringRef Parameter) {
  size_t Call = Body.find("pe_leaf(");
  ASSERT_NE(Call, llvm::StringRef::npos) << Body.str();
  size_t CallEnd = Body.find(';', Call);
  ASSERT_NE(CallEnd, llvm::StringRef::npos) << Body.str();
  llvm::StringRef Statement = Body.slice(Call, CallEnd);
  EXPECT_TRUE(Statement.contains(Parameter))
      << "pe_leaf call does not use " << Parameter.str() << ":\n"
      << Statement.str();
}

bool isCIdentifierChar(char C) {
  return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
}

size_t findIdentifier(llvm::StringRef Text, llvm::StringRef Name, size_t From) {
  while (true) {
    size_t Pos = Text.find(Name, From);
    if (Pos == llvm::StringRef::npos)
      return Pos;
    bool LeftBoundary = Pos == 0 || !isCIdentifierChar(Text[Pos - 1]);
    size_t End = Pos + Name.size();
    bool RightBoundary = End == Text.size() || !isCIdentifierChar(Text[End]);
    if (LeftBoundary && RightBoundary)
      return Pos;
    From = End;
  }
}

void expectNoLocalReadBeforeDefinition(llvm::StringRef Body) {
  struct LocalDecl {
    std::string Name;
    size_t End = 0;
  };
  std::vector<LocalDecl> Locals;
  size_t OpenBrace = Body.find('{');
  ASSERT_NE(OpenBrace, llvm::StringRef::npos) << Body.str();
  size_t LineStart = OpenBrace + 1;
  while (LineStart < Body.size()) {
    size_t LineEnd = Body.find('\n', LineStart);
    if (LineEnd == llvm::StringRef::npos)
      LineEnd = Body.size();
    llvm::StringRef Line = Body.slice(LineStart, LineEnd).trim();
    if (Line.ends_with(";") && !Line.contains('=') && !Line.contains('(')) {
      llvm::StringRef Declaration = Line.drop_back().trim();
      size_t TypeEnd = Declaration.find_first_of(" \t");
      llvm::StringRef Type = Declaration.take_front(TypeEnd);
      if (Type.ends_with("_t") || Type == "int" || Type == "long" ||
          Type == "short" || Type == "char" || Type == "float" ||
          Type == "double") {
        size_t NameEnd = Declaration.size();
        while (NameEnd > 0 && std::isspace(static_cast<unsigned char>(
                                  Declaration[NameEnd - 1])))
          --NameEnd;
        size_t NameStart = NameEnd;
        while (NameStart > 0 && isCIdentifierChar(Declaration[NameStart - 1]))
          --NameStart;
        llvm::StringRef Name = Declaration.slice(NameStart, NameEnd);
        if (!Name.empty())
          Locals.push_back({Name.str(), LineEnd});
      }
    }
    LineStart = LineEnd + 1;
  }

  std::set<llvm::StringRef> DeclaredNames;
  for (const LocalDecl &Local : Locals)
    DeclaredNames.insert(Local.Name);
  std::set<std::string> UndeclaredNames;
  for (size_t Pos = OpenBrace + 1; Pos + 1 < Body.size(); ++Pos) {
    if (Body[Pos] != 'v' ||
        !std::isdigit(static_cast<unsigned char>(Body[Pos + 1])) ||
        (Pos > 0 && isCIdentifierChar(Body[Pos - 1])))
      continue;
    size_t End = Pos + 2;
    while (End < Body.size() && isCIdentifierChar(Body[End]))
      ++End;
    llvm::StringRef Name = Body.slice(Pos, End);
    if (!DeclaredNames.count(Name))
      UndeclaredNames.insert(Name.str());
    Pos = End - 1;
  }

  std::vector<std::string> ReadBeforeDefinition;
  for (const LocalDecl &Local : Locals) {
    size_t Use = findIdentifier(Body, Local.Name, Local.End);
    if (Use == llvm::StringRef::npos)
      continue;
    size_t StatementStart = Body.rfind(';', Use);
    size_t OpenBrace = Body.rfind('{', Use);
    if (StatementStart == llvm::StringRef::npos ||
        (OpenBrace != llvm::StringRef::npos && OpenBrace > StatementStart))
      StatementStart = OpenBrace;
    StatementStart =
        StatementStart == llvm::StringRef::npos ? 0 : StatementStart + 1;
    size_t StatementEnd = Body.find(';', Use);
    size_t Equals = Body.find('=', StatementStart);
    bool IsDefinition = StatementEnd != llvm::StringRef::npos &&
                        Equals < StatementEnd && Use < Equals &&
                        Body.slice(StatementStart, Equals).trim() == Local.Name;
    if (!IsDefinition)
      ReadBeforeDefinition.push_back(Local.Name);
  }

  EXPECT_TRUE(ReadBeforeDefinition.empty())
      << "locals read before definition: "
      << llvm::join(ReadBeforeDefinition, ", ") << "\n"
      << Body.str();
  EXPECT_TRUE(UndeclaredNames.empty())
      << "undeclared locals: " << llvm::join(UndeclaredNames, ", ") << "\n"
      << Body.str();
}

void expectFrameBaseInitializedOnce(llvm::StringRef Body) {
  constexpr llvm::StringLiteral Declaration = "uintptr_t frame_base =";
  size_t Init = Body.find(Declaration);
  ASSERT_NE(Init, llvm::StringRef::npos) << Body.str();
  size_t InitEnd = Body.find(';', Init);
  ASSERT_NE(InitEnd, llvm::StringRef::npos) << Body.str();

  std::vector<std::string> Reassignments;
  size_t Use = findIdentifier(Body, "frame_base", InitEnd + 1);
  while (Use != llvm::StringRef::npos) {
    size_t StatementStart = Body.rfind(';', Use);
    size_t OpenBrace = Body.rfind('{', Use);
    if (StatementStart == llvm::StringRef::npos ||
        (OpenBrace != llvm::StringRef::npos && OpenBrace > StatementStart))
      StatementStart = OpenBrace;
    StatementStart =
        StatementStart == llvm::StringRef::npos ? 0 : StatementStart + 1;
    size_t StatementEnd = Body.find(';', Use);
    size_t Equals = Body.find('=', StatementStart);
    if (StatementEnd != llvm::StringRef::npos && Equals < StatementEnd &&
        Use < Equals &&
        Body.slice(StatementStart, Equals).trim() == "frame_base")
      Reassignments.push_back(Body.slice(StatementStart, StatementEnd).str());
    Use = findIdentifier(Body, "frame_base", Use + 10);
  }

  EXPECT_TRUE(Reassignments.empty())
      << "frame_base must remain the immutable entry stack pointer; found: "
      << llvm::join(Reassignments, ", ") << "\n"
      << Body.str();
}

std::string readTextFile(const fs::path &Path) {
  std::ifstream In(Path);
  return std::string(std::istreambuf_iterator<char>(In), {});
}

class COFFARMPipeline : public NeverDLiftTest {};

std::optional<size_t>
exportAddressEntryFileOffset(const llvm::object::COFFObjectFile &Obj,
                             llvm::StringRef Name) {
  const llvm::object::export_directory_table_entry *Table =
      Obj.getExportTable();
  if (!Table)
    return std::nullopt;

  uint32_t Index = 0;
  bool Found = false;
  for (auto I = Obj.export_directory_begin(), E = Obj.export_directory_end();
       I != E; ++I) {
    llvm::StringRef ExportName;
    if (llvm::Error Err = I->getSymbolName(ExportName)) {
      llvm::consumeError(std::move(Err));
      continue;
    }
    if (ExportName != Name)
      continue;
    uint32_t Ordinal = 0;
    if (llvm::Error Err = I->getOrdinal(Ordinal)) {
      llvm::consumeError(std::move(Err));
      return std::nullopt;
    }
    if (Ordinal < uint32_t(Table->OrdinalBase))
      return std::nullopt;
    Index = Ordinal - uint32_t(Table->OrdinalBase);
    Found = true;
    break;
  }
  if (!Found || Index >= uint32_t(Table->AddressTableEntries))
    return std::nullopt;

  auto AddressTableOff =
      rvaFileOffset(Obj, uint32_t(Table->ExportAddressTableRVA));
  if (!AddressTableOff)
    return std::nullopt;
  uint64_t EntryOff =
      *AddressTableOff +
      uint64_t(Index) * sizeof(llvm::object::export_address_table_entry);
  if (!rangeInBounds(EntryOff, sizeof(llvm::object::export_address_table_entry),
                     Obj.getData().size()))
    return std::nullopt;
  return static_cast<size_t>(EntryOff);
}

std::optional<uint32_t> oddDataRVA(const llvm::object::COFFObjectFile &Obj) {
  for (const llvm::object::SectionRef &SecRef : Obj.sections()) {
    auto NameOrErr = SecRef.getName();
    if (!NameOrErr) {
      llvm::consumeError(NameOrErr.takeError());
      continue;
    }
    if (*NameOrErr != ".data")
      continue;
    const llvm::object::coff_section *Sec = Obj.getCOFFSection(SecRef);
    if (!Sec)
      return std::nullopt;
    uint32_t Size = Sec->VirtualSize ? uint32_t(Sec->VirtualSize)
                                     : uint32_t(Sec->SizeOfRawData);
    if (Size < 2)
      return std::nullopt;
    return uint32_t(Sec->VirtualAddress) + 1u;
  }
  return std::nullopt;
}

constexpr size_t PDataEntrySize = 2 * sizeof(uint32_t);

std::optional<std::array<size_t, 3>>
firstThreePDataEntryOffsets(const llvm::object::COFFObjectFile &Obj) {
  const llvm::object::data_directory *Dir =
      Obj.getDataDirectory(llvm::COFF::EXCEPTION_TABLE);
  if (!Dir || static_cast<size_t>(Dir->Size) / PDataEntrySize < 3u)
    return std::nullopt;
  auto TableOff = rvaFileOffset(Obj, Dir->RelativeVirtualAddress);
  if (!TableOff ||
      !rangeInBounds(*TableOff, 3 * PDataEntrySize, Obj.getData().size()))
    return std::nullopt;
  return std::array<size_t, 3>{*TableOff, *TableOff + PDataEntrySize,
                               *TableOff + 2 * PDataEntrySize};
}

void swapPDataEntries(std::vector<uint8_t> &Bytes, size_t A, size_t B) {
  std::array<uint8_t, PDataEntrySize> Temp;
  std::memcpy(Temp.data(), Bytes.data() + A, PDataEntrySize);
  std::memcpy(Bytes.data() + A, Bytes.data() + B, PDataEntrySize);
  std::memcpy(Bytes.data() + B, Temp.data(), PDataEntrySize);
}

struct FullFunctionExpectation {
  va_t Addr;
  uint32_t Length;
};

std::optional<FullFunctionExpectation>
readAArch64PackedFull(const std::vector<uint8_t> &Bytes, uint64_t ImageBase,
                      size_t EntryOff) {
  if (!rangeInBounds(EntryOff, PDataEntrySize, Bytes.size()))
    return std::nullopt;
  uint32_t Begin = readLE<uint32_t>(Bytes.data() + EntryOff);
  uint32_t Unwind =
      readLE<uint32_t>(Bytes.data() + EntryOff + sizeof(uint32_t));
  if ((Unwind & 3u) != 1u)
    return std::nullopt;
  uint32_t Length = ((Unwind & 0x1ffcu) >> 2) * 4u;
  if (Length == 0 || Begin > InvalidVA - ImageBase)
    return std::nullopt;
  return FullFunctionExpectation{ImageBase + Begin, Length};
}

void expectFullFunctionPresent(const BinaryImage &Img,
                               const FullFunctionExpectation &Expected) {
  EXPECT_NE(
      std::find(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end(),
                std::make_pair(Expected.Addr, Expected.Addr + Expected.Length)),
      Img.KnownCodeRanges.end());
  EXPECT_NE(std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                         [&](const Symbol &S) {
                           return S.IsFunc && S.Addr == Expected.Addr &&
                                  S.Size == Expected.Length;
                         }),
            Img.Symbols.end());
}

std::optional<size_t>
findPDataEntryOffsetByFlag(const llvm::object::COFFObjectFile &Obj,
                           uint32_t WantedFlag) {
  const llvm::object::data_directory *Dir =
      Obj.getDataDirectory(llvm::COFF::EXCEPTION_TABLE);
  if (!Dir)
    return std::nullopt;
  auto TableOff = rvaFileOffset(Obj, Dir->RelativeVirtualAddress);
  if (!TableOff)
    return std::nullopt;
  constexpr size_t EntrySize = 2 * sizeof(uint32_t);
  size_t Count = Dir->Size / EntrySize;
  for (size_t I = 0; I < Count; ++I) {
    size_t EntryOff = *TableOff + I * EntrySize;
    if (!rangeInBounds(EntryOff, EntrySize, Obj.getData().size()))
      break;
    const auto *Entry =
        reinterpret_cast<const uint8_t *>(Obj.getData().data()) + EntryOff;
    uint32_t UnwindWord = readLE<uint32_t>(Entry + sizeof(uint32_t));
    if (UnwindWord != 0 && (UnwindWord & 3u) == WantedFlag)
      return EntryOff;
  }
  return std::nullopt;
}

std::optional<size_t>
findUnpackedPDataEntryOffset(const llvm::object::COFFObjectFile &Obj) {
  return findPDataEntryOffsetByFlag(Obj, 0u);
}

std::optional<size_t> xdataFileOffset(const std::vector<uint8_t> &Bytes,
                                      const llvm::object::COFFObjectFile &Obj,
                                      const BinaryImage &Img,
                                      size_t PDataEntryOff) {
  if (!rangeInBounds(PDataEntryOff, 2 * sizeof(uint32_t), Bytes.size()))
    return std::nullopt;
  uint32_t UnwindWord =
      readLE<uint32_t>(Bytes.data() + PDataEntryOff + sizeof(uint32_t));
  uint32_t XDataRVA = UnwindWord & ~3u;
  if (XDataRVA > InvalidVA - Obj.getImageBase())
    return std::nullopt;
  va_t XDataVA = Obj.getImageBase() + XDataRVA;
  for (const Section &Sec : Img.Sections) {
    if (!Sec.contains(XDataVA))
      continue;
    uint64_t Delta = XDataVA - Sec.VA;
    if (Delta > InvalidVA - Sec.FileOff)
      return std::nullopt;
    uint64_t Off = Sec.FileOff + Delta;
    if (!rangeInBounds(Off, sizeof(uint32_t), Bytes.size()))
      return std::nullopt;
    return static_cast<size_t>(Off);
  }
  return std::nullopt;
}

uint64_t maxFunctionSize(const BinaryImage &Img) {
  uint64_t Max = 0;
  for (const auto &Sym : Img.Symbols)
    if (Sym.IsFunc)
      Max = std::max(Max, Sym.Size);
  return Max;
}

void expectAllFunctionRangesInsideExecutableSegments(const BinaryImage &Img) {
  for (const auto &[Begin, End] : Img.KnownCodeRanges) {
    ASSERT_LT(Begin, End);
    const Segment *Seg = Img.getSegmentFor(Begin);
    ASSERT_NE(Seg, nullptr);
    ASSERT_TRUE(Seg->isExecutable());
    ASSERT_LE(End - Seg->VA, std::min<uint64_t>(Seg->Size, Seg->Data.size()));
  }
}

void expectFullPDataStartsHaveBoundedSymbols(const BinaryImage &Img,
                                             const fs::path &Path) {
  std::vector<uint8_t> Bytes = readFile(Path);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  const llvm::object::data_directory *Dir =
      Obj->getDataDirectory(llvm::COFF::EXCEPTION_TABLE);
  ASSERT_NE(Dir, nullptr);

  uintptr_t TablePtr = 0;
  llvm::Error Err = Obj->getRvaPtr(Dir->RelativeVirtualAddress, TablePtr);
  ASSERT_FALSE(static_cast<bool>(Err)) << llvm::toString(std::move(Err));
  uintptr_t FileBegin = reinterpret_cast<uintptr_t>(Obj->getData().data());
  uintptr_t FileEnd = FileBegin + Obj->getData().size();
  ASSERT_GE(TablePtr, FileBegin);
  ASSERT_LE(TablePtr, FileEnd);
  size_t Available = FileEnd - TablePtr;
  constexpr size_t EntrySize = 2 * sizeof(uint32_t);
  size_t Count = std::min<size_t>(Dir->Size, Available) / EntrySize;
  const auto *Table = reinterpret_cast<const uint8_t *>(TablePtr);
  size_t FullCount = 0;
  size_t SymbolEligibleCount = 0;

  for (size_t I = 0; I < Count; ++I) {
    SCOPED_TRACE(I);
    uint32_t BeginWord = readLE<uint32_t>(Table + I * EntrySize);
    uint32_t UnwindWord =
        readLE<uint32_t>(Table + I * EntrySize + sizeof(uint32_t));
    uint32_t Flag = UnwindWord & 3u;
    if ((BeginWord == 0 && UnwindWord == 0) || Flag == 3u)
      continue;

    uint32_t Length = 0;
    bool IsFragment = Flag == 2u;
    if (Flag == 1u || Flag == 2u) {
      uint32_t Unit = Img.Arch == Arch::ARM ? 2u : 4u;
      Length = ((UnwindWord & 0x1ffcu) >> 2) * Unit;
    } else {
      uintptr_t XDataPtr = 0;
      llvm::Error XErr = Obj->getRvaPtr(UnwindWord & ~3u, XDataPtr);
      if (XErr) {
        llvm::consumeError(std::move(XErr));
        continue;
      }
      if (XDataPtr < FileBegin || XDataPtr > FileEnd ||
          static_cast<size_t>(FileEnd - XDataPtr) < sizeof(uint32_t))
        continue;
      uint32_t Header =
          readLE<uint32_t>(reinterpret_cast<const uint8_t *>(XDataPtr));
      if (((Header >> 18) & 3u) != 0)
        continue;
      uint32_t Unit = Img.Arch == Arch::ARM ? 2u : 4u;
      Length = (Header & 0x3ffffu) * Unit;
      if (Img.Arch == Arch::ARM)
        IsFragment = (Header & (1u << 22)) != 0;
    }
    if (Length == 0 || IsFragment)
      continue;

    uint64_t Begin = normalizeCodeAddress(BeginWord, Img.Arch, Img.Mode);
    if (Begin > InvalidVA - Obj->getImageBase())
      continue;
    va_t Addr = Obj->getImageBase() + Begin;
    if (Length > InvalidVA - Addr)
      continue;
    const Segment *Seg = Img.getSegmentFor(Addr);
    if (!Seg || !Seg->isExecutable())
      continue;
    uint64_t Usable = std::min<uint64_t>(Seg->Size, Seg->Data.size());
    if (Usable > InvalidVA - Seg->VA || Addr + Length > Seg->VA + Usable)
      continue;

    ++FullCount;
    size_t Off = static_cast<size_t>(Addr - Seg->VA);
    if (!checkPrologueAtOffset(*Seg, Off, Img.Arch))
      continue;
    ++SymbolEligibleCount;
    auto Sym = std::find_if(
        Img.Symbols.begin(), Img.Symbols.end(), [&](const Symbol &S) {
          return S.IsFunc && S.Addr == Addr && S.Size == Length;
        });
    EXPECT_NE(Sym, Img.Symbols.end())
        << "missing exact function symbol at 0x" << std::hex << Addr
        << " with size 0x" << Length;
  }
  EXPECT_GE(FullCount, 2u);
  EXPECT_GE(SymbolEligibleCount, 1u);
}

TEST_F(COFFARMFormat, AArch64UsesEightBytePDataRecords) {
  const fs::path Path = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Path))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;

  EXPECT_EQ(Img.Format, BinaryFormat::COFF);
  EXPECT_EQ(Img.Arch, Arch::AArch64);
  EXPECT_EQ(Img.Bits, Bitness::Bits64);
  EXPECT_EQ(Img.Mode, InstructionMode::Default);
  expectAllFunctionRangesInsideExecutableSegments(Img);
  EXPECT_GE(Img.KnownCodeRanges.size(), 2u);
  expectFullPDataStartsHaveBoundedSymbols(Img, Path);
  EXPECT_GT(maxFunctionSize(Img), 0u);
  EXPECT_LT(maxFunctionSize(Img), 4096u);
}

TEST_F(COFFARMFormat, ARM32IsThumbWithNormalizedEntryAndBoundedRanges) {
  const fs::path Path = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Path))
    GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";

  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;

  EXPECT_EQ(Img.Format, BinaryFormat::COFF);
  EXPECT_EQ(Img.Arch, Arch::ARM);
  EXPECT_EQ(Img.Bits, Bitness::Bits32);
  EXPECT_EQ(Img.Mode, InstructionMode::Thumb);
  EXPECT_EQ(Img.Entry & 1, 0u);
  expectAllFunctionRangesInsideExecutableSegments(Img);
  EXPECT_GE(Img.KnownCodeRanges.size(), 2u);
  expectFullPDataStartsHaveBoundedSymbols(Img, Path);
  EXPECT_GT(maxFunctionSize(Img), 0u);
  EXPECT_LT(maxFunctionSize(Img), 4096u);
}

TEST_F(COFFARMFormat, ARM32CodeExportsAreNormalizedAndSerializedAsThumb) {
  const fs::path Path = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Path))
    GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Path);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto RawLeaf = rawExportRVA(*Obj, "pe_leaf");
  auto RawStacky = rawExportRVA(*Obj, "pe_stacky");
  ASSERT_TRUE(RawLeaf.has_value());
  ASSERT_TRUE(RawStacky.has_value());
  EXPECT_EQ(*RawLeaf & 1u, 1u);
  EXPECT_EQ(*RawStacky & 1u, 1u);

  auto ImgOrErr = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  for (llvm::StringRef Name :
       {llvm::StringRef("pe_leaf"), llvm::StringRef("pe_stacky")}) {
    uint32_t RawRVA = Name == "pe_leaf" ? *RawLeaf : *RawStacky;
    const Export *Exp = findExport(Img, Name);
    ASSERT_NE(Exp, nullptr);
    EXPECT_EQ(Exp->Addr, Img.Base + clearThumbBit(RawRVA));
    EXPECT_EQ(Exp->Addr & 1u, 0u);
    EXPECT_EQ(std::count_if(Img.Exports.begin(), Img.Exports.end(),
                            [&](const Export &E) { return E.Name == Name; }),
              1);
    EXPECT_EQ(serializeExportAddress(Img, Exp->Addr), Exp->Addr | 1u);
  }
  EXPECT_EQ(std::count_if(
                Img.Symbols.begin(), Img.Symbols.end(),
                [](const Symbol &S) { return S.IsFunc && (S.Addr & 1u) != 0; }),
            0);
}

TEST_F(COFFARMFormat, ARM32OddDataExportRemainsData) {
  const fs::path Source = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = exportAddressEntryFileOffset(*Obj, "pe_stacky");
  auto DataRVA = oddDataRVA(*Obj);
  ASSERT_TRUE(EntryOff.has_value());
  ASSERT_TRUE(DataRVA.has_value());
  writeLE<uint32_t>(Bytes.data() + *EntryOff, *DataRVA);

  fs::path Mutated = writeMutation("odd-data-export.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  const BinaryImage &Img = *ImgOrErr;
  const Export *Exp = findExport(Img, "pe_stacky");
  ASSERT_NE(Exp, nullptr);
  va_t Expected = Img.Base + *DataRVA;
  EXPECT_EQ(Expected & 1u, 1u);
  EXPECT_EQ(Exp->Addr, Expected);
  const Segment *Seg = Img.getSegmentFor(Exp->Addr);
  ASSERT_NE(Seg, nullptr);
  EXPECT_FALSE(Seg->isExecutable());
  EXPECT_EQ(serializeExportAddress(Img, Exp->Addr), Expected);
}

TEST_F(COFFARMFormat, PartialARM64PDataRecordIsIgnored) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  size_t DirOff = exceptionDirectoryFileOffset(*Obj);
  ASSERT_LE(DirOff + sizeof(llvm::object::data_directory), Bytes.size());
  writeLE<uint32_t>(
      Bytes.data() + DirOff + offsetof(llvm::object::data_directory, Size), 4u);

  fs::path Mutated = writeMutation("partial-pdata.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_TRUE(ImgOrErr->KnownCodeRanges.empty());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, InvalidMiddleARM64XDataDoesNotStopLaterRecords) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOffsets = firstThreePDataEntryOffsets(*Obj);
  ASSERT_TRUE(EntryOffsets.has_value());
  auto [FirstOff, MiddleOff, LastOff] = *EntryOffsets;
  ASSERT_EQ(readLE<uint32_t>(Bytes.data() + MiddleOff + sizeof(uint32_t)) & 3u,
            0u);

  swapPDataEntries(Bytes, FirstOff, LastOff);
  writeLE<uint32_t>(Bytes.data() + MiddleOff + sizeof(uint32_t), 0x7ffffffcu);
  auto Later = readAArch64PackedFull(Bytes, Obj->getImageBase(), LastOff);
  ASSERT_TRUE(Later.has_value());

  fs::path Mutated = writeMutation("invalid-xdata-rva.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  expectFullFunctionPresent(*ImgOrErr, *Later);
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, TruncatedARM64XDataIsSkipped) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOffsets = firstThreePDataEntryOffsets(*Obj);
  ASSERT_TRUE(EntryOffsets.has_value());
  auto [FirstOff, MiddleOff, LaterOff] = *EntryOffsets;
  (void)FirstOff;
  ASSERT_EQ(readLE<uint32_t>(Bytes.data() + MiddleOff + sizeof(uint32_t)) & 3u,
            0u);

  const llvm::object::coff_section *PData = nullptr;
  for (const llvm::object::SectionRef &SecRef : Obj->sections()) {
    auto NameOrErr = SecRef.getName();
    if (!NameOrErr) {
      llvm::consumeError(NameOrErr.takeError());
      continue;
    }
    if (*NameOrErr == ".pdata") {
      PData = Obj->getCOFFSection(SecRef);
      break;
    }
  }
  ASSERT_NE(PData, nullptr);
  uint32_t PDataVA = PData->VirtualAddress;
  uint32_t PDataRawOff = PData->PointerToRawData;
  uint32_t PDataRawSize = PData->SizeOfRawData;
  ASSERT_GE(PDataRawSize, 8u);
  uint32_t XDataDelta = (PDataRawSize - 4u) & ~3u;
  uint32_t XDataRVA = PDataVA + XDataDelta;
  size_t PDataHeaderOff = static_cast<size_t>(
      reinterpret_cast<const char *>(PData) - Obj->getData().data());
  ASSERT_TRUE(rangeInBounds(PDataHeaderOff, sizeof(*PData), Bytes.size()));
  writeLE<uint32_t>(Bytes.data() + PDataHeaderOff +
                        offsetof(llvm::object::coff_section, VirtualSize),
                    XDataDelta + 4u);
  writeLE<uint32_t>(Bytes.data() + PDataHeaderOff +
                        offsetof(llvm::object::coff_section, SizeOfRawData),
                    XDataDelta + 3u);
  writeLE<uint32_t>(Bytes.data() + MiddleOff + sizeof(uint32_t), XDataRVA);

  auto Later = readAArch64PackedFull(Bytes, Obj->getImageBase(), LaterOff);
  ASSERT_TRUE(Later.has_value());
  size_t TruncatedSize = static_cast<size_t>(PDataRawOff) + XDataDelta + 3u;
  ASSERT_LT(TruncatedSize, Bytes.size());
  Bytes.resize(TruncatedSize);

  auto TruncatedObj = createCOFFObject(Bytes);
  ASSERT_NE(TruncatedObj, nullptr);
  uintptr_t XDataPtr = 0;
  llvm::Error XDataErr = TruncatedObj->getRvaPtr(XDataRVA, XDataPtr);
  ASSERT_FALSE(static_cast<bool>(XDataErr))
      << llvm::toString(std::move(XDataErr));
  uintptr_t FileEnd = reinterpret_cast<uintptr_t>(Bytes.data()) + Bytes.size();
  ASSERT_EQ(FileEnd - XDataPtr, 3u);

  fs::path Mutated = writeMutation("truncated-xdata.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Later->Addr, Later->Addr + Later->Length)),
            ImgOrErr->KnownCodeRanges.end());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ReservedMiddleARM64RecordDoesNotStopLaterRecords) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOffsets = firstThreePDataEntryOffsets(*Obj);
  ASSERT_TRUE(EntryOffsets.has_value());
  auto [FirstOff, MiddleOff, LastOff] = *EntryOffsets;

  swapPDataEntries(Bytes, FirstOff, LastOff);
  uint32_t MiddleUnwind =
      readLE<uint32_t>(Bytes.data() + MiddleOff + sizeof(uint32_t));
  writeLE<uint32_t>(Bytes.data() + MiddleOff + sizeof(uint32_t),
                    (MiddleUnwind & ~3u) | 3u);

  auto Later = readAArch64PackedFull(Bytes, Obj->getImageBase(), LastOff);
  ASSERT_TRUE(Later.has_value());

  fs::path Mutated = writeMutation("reserved-middle-record.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  expectFullFunctionPresent(*ImgOrErr, *Later);
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ZeroARM64PackedLengthIsSkipped) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  auto BaselineOrErr = loadBinary(Source);
  ASSERT_TRUE(static_cast<bool>(BaselineOrErr))
      << llvm::toString(BaselineOrErr.takeError());
  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findPDataEntryOffsetByFlag(*Obj, 1u);
  ASSERT_TRUE(EntryOff.has_value());
  writeLE<uint32_t>(Bytes.data() + *EntryOff + sizeof(uint32_t), 1u);

  fs::path Mutated = writeMutation("zero-packed-length.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_LT(ImgOrErr->KnownCodeRanges.size(),
            BaselineOrErr->KnownCodeRanges.size());
  EXPECT_FALSE(ImgOrErr->KnownCodeRanges.empty());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, OverflowingARM64UnpackedLengthIsSkipped) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  auto BaselineOrErr = loadBinary(Source);
  ASSERT_TRUE(static_cast<bool>(BaselineOrErr))
      << llvm::toString(BaselineOrErr.takeError());
  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findUnpackedPDataEntryOffset(*Obj);
  ASSERT_TRUE(EntryOff.has_value());
  auto XDataOff = xdataFileOffset(Bytes, *Obj, *BaselineOrErr, *EntryOff);
  ASSERT_TRUE(XDataOff.has_value());
  writeLE<uint32_t>(Bytes.data() + *XDataOff, 0x3ffffu);

  fs::path Mutated = writeMutation("overflowing-xdata-length.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_LT(ImgOrErr->KnownCodeRanges.size(),
            BaselineOrErr->KnownCodeRanges.size());
  EXPECT_FALSE(ImgOrErr->KnownCodeRanges.empty());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ARM32PackedFragmentIsRangeOnly) {
  const fs::path Source = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findPDataEntryOffsetByFlag(*Obj, 1u);
  ASSERT_TRUE(EntryOff.has_value());
  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + clearThumbBit(BeginWord);
  constexpr uint32_t Length = 4;
  writeLE<uint32_t>(Bytes.data() + *EntryOff + sizeof(uint32_t),
                    ((Length / 2) << 2) | 2u);

  fs::path Mutated = writeMutation("arm32-packed-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_EQ(
      std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                   [&](const Symbol &S) { return S.IsFunc && S.Addr == Addr; }),
      ImgOrErr->Symbols.end());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ARM64PackedFragmentIsRangeOnly) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findPDataEntryOffsetByFlag(*Obj, 1u);
  ASSERT_TRUE(EntryOff.has_value());
  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + BeginWord;
  constexpr uint32_t Length = 4;
  writeLE<uint32_t>(Bytes.data() + *EntryOff + sizeof(uint32_t),
                    ((Length / 4) << 2) | 2u);

  fs::path Mutated = writeMutation("arm64-packed-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_EQ(
      std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                   [&](const Symbol &S) { return S.IsFunc && S.Addr == Addr; }),
      ImgOrErr->Symbols.end());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ARM32UnpackedFragmentIsRangeOnly) {
  const fs::path Source = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";

  auto BaselineOrErr = loadBinary(Source);
  ASSERT_TRUE(static_cast<bool>(BaselineOrErr))
      << llvm::toString(BaselineOrErr.takeError());
  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findUnpackedPDataEntryOffset(*Obj);
  ASSERT_TRUE(EntryOff.has_value());
  auto XDataOff = xdataFileOffset(Bytes, *Obj, *BaselineOrErr, *EntryOff);
  ASSERT_TRUE(XDataOff.has_value());
  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + clearThumbBit(BeginWord);
  constexpr uint32_t Length = 4;
  writeLE<uint32_t>(Bytes.data() + *XDataOff, (Length / 2) | (1u << 22));

  fs::path Mutated = writeMutation("arm32-unpacked-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_EQ(
      std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                   [&](const Symbol &S) { return S.IsFunc && S.Addr == Addr; }),
      ImgOrErr->Symbols.end());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, ARM64CorrespondingXDataBitIsNotFragment) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  auto BaselineOrErr = loadBinary(Source);
  ASSERT_TRUE(static_cast<bool>(BaselineOrErr))
      << llvm::toString(BaselineOrErr.takeError());
  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findUnpackedPDataEntryOffset(*Obj);
  ASSERT_TRUE(EntryOff.has_value());
  auto XDataOff = xdataFileOffset(Bytes, *Obj, *BaselineOrErr, *EntryOff);
  ASSERT_TRUE(XDataOff.has_value());
  uint32_t BeginWord = readLE<uint32_t>(Bytes.data() + *EntryOff);
  va_t Addr = Obj->getImageBase() + BeginWord;
  constexpr uint32_t Length = 4;
  writeLE<uint32_t>(Bytes.data() + *XDataOff, (Length / 4) | (1u << 22));

  fs::path Mutated = writeMutation("arm64-not-fragment.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_NE(std::find(ImgOrErr->KnownCodeRanges.begin(),
                      ImgOrErr->KnownCodeRanges.end(),
                      std::make_pair(Addr, Addr + Length)),
            ImgOrErr->KnownCodeRanges.end());
  EXPECT_NE(std::find_if(ImgOrErr->Symbols.begin(), ImgOrErr->Symbols.end(),
                         [&](const Symbol &S) {
                           return S.IsFunc && S.Addr == Addr &&
                                  S.Size == Length;
                         }),
            ImgOrErr->Symbols.end());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMFormat, UnsupportedARM64XDataVersionIsSkipped) {
  const fs::path Source = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Source))
    GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";

  auto BaselineOrErr = loadBinary(Source);
  ASSERT_TRUE(static_cast<bool>(BaselineOrErr))
      << llvm::toString(BaselineOrErr.takeError());
  std::vector<uint8_t> Bytes = readFile(Source);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  auto EntryOff = findUnpackedPDataEntryOffset(*Obj);
  ASSERT_TRUE(EntryOff.has_value());
  auto XDataOff = xdataFileOffset(Bytes, *Obj, *BaselineOrErr, *EntryOff);
  ASSERT_TRUE(XDataOff.has_value());
  uint32_t Header = readLE<uint32_t>(Bytes.data() + *XDataOff);
  Header = (Header & ~(3u << 18)) | (1u << 18);
  writeLE<uint32_t>(Bytes.data() + *XDataOff, Header);

  fs::path Mutated = writeMutation("arm64-xdata-version.exe", Bytes);
  auto ImgOrErr = loadBinary(Mutated);
  ASSERT_TRUE(static_cast<bool>(ImgOrErr))
      << llvm::toString(ImgOrErr.takeError());
  EXPECT_LT(ImgOrErr->KnownCodeRanges.size(),
            BaselineOrErr->KnownCodeRanges.size());
  EXPECT_FALSE(ImgOrErr->KnownCodeRanges.empty());
  expectAllFunctionRangesInsideExecutableSegments(*ImgOrErr);
}

TEST_F(COFFARMPipeline, ARM32ThumbLiftAndDecompile) {
  const fs::path Path = fixture("test_patch_coff_arm.exe");
  if (!fs::exists(Path)) {
    if (!fs::exists(fixture("test_patch_coff_arm.obj")))
      GTEST_SKIP() << "ARM32 PE fixture not built (lld-link unavailable)";
    FAIL() << "ARM32 PE object exists but linked fixture is missing";
    return;
  }

  verifyAllStages(Path);

  RunResult Low = liftToLowIR(Path);
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  auto Leaf = lowFunctionBody(Low.out, "pe_leaf");
  auto Stacky = lowFunctionBody(Low.out, "pe_stacky");
  ASSERT_TRUE(Leaf.has_value()) << Low.out;
  ASSERT_TRUE(Stacky.has_value()) << Low.out;
  expectLeafSemantics(*Leaf);
  expectStackySemantics(*Stacky);
  EXPECT_NE(Stacky->find("INT_SUB reg:0x34:4 reg:0x34:4 cst:0x8:4"),
            std::string::npos)
      << "wide Thumb push must reserve both saved registers on SP:\n"
      << *Stacky;
  EXPECT_NE(Stacky->find("INT_ADD reg:0x34:4 reg:0x34:4 cst:0x8:4"),
            std::string::npos)
      << "wide Thumb pop must release both saved registers from SP:\n"
      << *Stacky;
  EXPECT_EQ(Stacky->find("INT_SUB reg:0x2C:4 reg:0x2C:4 cst:0x4:4"),
            std::string::npos)
      << "push.w was decoded as a generic STMDB using R11 as its base:\n"
      << *Stacky;
  expectNoOddFunctionAddresses(Low.out);
  verifyNoUnlifted(Path);

  RunResult Med = liftToMedIR(Path);
  ASSERT_EQ(Med.exitCode, 0) << Med.err;
  EXPECT_TRUE(Med.out.find("func pe_stacky @ 0x401010 cc=3 FrameSize=400") !=
              std::string::npos)
      << Med.out;

  RunResult Decompile = decompileToHighC(Path);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  const fs::path CPath = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CPath));
  std::string C = readTextFile(CPath);
  auto LeafC = cFunctionBody(C, "pe_leaf");
  auto StackyC = cFunctionBody(C, "pe_stacky");
  ASSERT_TRUE(LeafC.has_value()) << C;
  ASSERT_TRUE(StackyC.has_value()) << C;
  EXPECT_NE(StackyC->find("pe_leaf("), std::string::npos) << *StackyC;
  EXPECT_EQ(StackyC->find("arg4"), std::string::npos)
      << "the saved return address must not become a fifth parameter:\n"
      << *StackyC;
  expectLeafCallResultStored(*StackyC);
  expectNoLocalReadBeforeDefinition(*StackyC);
  expectFrameBaseInitializedOnce(*StackyC);

  const fs::path IncludeDir = tmpFile("arm-include");
  fs::create_directories(IncludeDir);
  std::ofstream StringHeader(IncludeDir / "string.h");
  StringHeader << "#include <stddef.h>\n"
                  "void *memcpy(void *, const void *, size_t);\n";
  StringHeader.close();
  ASSERT_TRUE(StringHeader.good());
  RunResult Syntax =
      exec("clang",
           {"-std=c11", "-target", "thumbv7-pc-windows-msvc", "-ffreestanding",
            "-I", IncludeDir.string(), "-fsyntax-only", CPath.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err;
}

TEST_F(COFFARMPipeline, AArch64LiftAndDecompile) {
  const fs::path Path = fixture("test_patch_coff_a64.exe");
  if (!fs::exists(Path)) {
    if (!fs::exists(fixture("test_patch_coff_a64.obj")))
      GTEST_SKIP() << "AArch64 PE fixture not built (lld-link unavailable)";
    FAIL() << "AArch64 PE object exists but linked fixture is missing";
    return;
  }

  verifyAllStages(Path);

  RunResult Low = liftToLowIR(Path);
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  auto Leaf = lowFunctionBody(Low.out, "pe_leaf");
  auto Stacky = lowFunctionBody(Low.out, "pe_stacky");
  ASSERT_TRUE(Leaf.has_value()) << Low.out;
  ASSERT_TRUE(Stacky.has_value()) << Low.out;
  expectLeafSemantics(*Leaf);
  expectStackySemantics(*Stacky);
  verifyNoUnlifted(Path);

  RunResult Decompile = decompileToHighC(Path);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  const fs::path CPath = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CPath));
  std::string C = readTextFile(CPath);
  auto LeafC = cFunctionBody(C, "pe_leaf");
  auto StackyC = cFunctionBody(C, "pe_stacky");
  ASSERT_TRUE(LeafC.has_value()) << C;
  ASSERT_TRUE(StackyC.has_value()) << C;
  EXPECT_NE(StackyC->find("pe_leaf("), std::string::npos) << *StackyC;
  expectLeafCallResultStored(*StackyC);
  expectLeafCallUsesParameter(*StackyC, "arg0");
  expectNoLocalReadBeforeDefinition(*StackyC);
  expectFrameBaseInitializedOnce(*StackyC);
}

TEST_F(COFFARMPipeline, X86_64StackFrameUsesDefinedEntrySP) {
  const fs::path Path = fixture("test_roundtrip.o");
  ASSERT_TRUE(fs::exists(Path));

  RunResult Decompile = decompileToHighC(Path);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  std::string C = readTextFile(tmpFile("decompiled_high.c"));
  auto LoopC = cFunctionBody(C, "rt_for_loop");
  auto StartC = cFunctionBody(C, "start");
  ASSERT_TRUE(LoopC.has_value()) << C;
  ASSERT_TRUE(StartC.has_value()) << C;
  for (llvm::StringRef Body :
       {llvm::StringRef(*LoopC), llvm::StringRef(*StartC)}) {
    EXPECT_TRUE(Body.contains("stack_storage[")) << Body.str();
    expectNoLocalReadBeforeDefinition(Body);
    expectFrameBaseInitializedOnce(Body);
  }
}

TEST_F(COFFARMPipeline, HighCStackStorageIsAlignedBoundedAndAliasSafe) {
  HighFunc Func;
  Func.Name = "stack_bounds";
  Func.FrameSize = 5;
  Func.FrameHeadroom = 8;
  Func.ReturnType = NdType::makeInt(4);

  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.TheArch = Arch::AArch64;
  SP.Id = 1;
  SP.Size = 8;
  SP.RegOff = getTargetRegInfo(Arch::AArch64).StackPointer;
  auto Addr = HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeVar(SP), HighExpr::makeConst(4, 8));
  auto Store = std::make_shared<HighExpr>();
  Store->Kind = ExprKind::Store;
  Store->Type = NdType::makeInt(4);
  Store->Operands.push_back(HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeVar(SP), HighExpr::makeConst(4, 8)));
  Store->Operands.push_back(HighExpr::makeConst(7, 4));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeBinop(
      NdOp::INT_ADD, Store,
      HighExpr::makeLoad(Addr, NdType::makeInt(4)));
  Func.Body.push_back(std::move(Ret));

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
  OS.flush();

  constexpr llvm::StringLiteral Storage =
      "_Alignas(16) uint8_t stack_storage[32];";
  size_t StorageAt = C.find(Storage);
  ASSERT_NE(StorageAt, std::string::npos) << C;
  EXPECT_EQ(C.find(Storage, StorageAt + Storage.size()), std::string::npos)
      << C;
  EXPECT_NE(C.find("const uintptr_t frame_base = "
                   "(uintptr_t)(stack_storage + 16);"),
            std::string::npos)
      << C;
  size_t FrameBaseAt = C.find("const uintptr_t frame_base =");
  ASSERT_NE(FrameBaseAt, std::string::npos) << C;
  EXPECT_EQ(C.find("const uintptr_t frame_base =", FrameBaseAt + 1),
            std::string::npos)
      << C;
  EXPECT_NE(C.find("neverd_mem_load_"), std::string::npos) << C;
  EXPECT_NE(C.find("neverd_mem_store_"), std::string::npos) << C;
  EXPECT_NE(C.find("memcpy(&value, (const void *)address, sizeof(value));"),
            std::string::npos)
      << C;
  EXPECT_EQ(C.find("*(int32_t*)"), std::string::npos) << C;
  EXPECT_NE(C.find("frame_base + 4"), std::string::npos) << C;
  EXPECT_NE(C.find("frame_base - 4"), std::string::npos) << C;

  const fs::path CPath = tmpFile("stack_bounds.c");
  std::ofstream Out(CPath);
  Out << C;
  Out.close();
  ASSERT_TRUE(Out.good());
  RunResult Syntax = exec("clang", {"-std=c11", "-fsyntax-only",
                                    CPath.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
}

TEST_F(COFFARMPipeline, StackBoundsSplitSlotStraddlingEntrySP) {
  constexpr Arch TheArch = Arch::X64;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "straddling_stack_slot";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Low.Entry;
  Block.EndAddr = Low.Entry + 1;

  NdVar Addr = NdVar::tmp(TmpBase, 8);
  LowOp FormAddr;
  FormAddr.Opcode = NdOp::INT_ADD;
  FormAddr.Output = Addr;
  FormAddr.addInput(NdVar::reg(TRI.StackPointer, 8));
  FormAddr.addInput(NdVar::cst(static_cast<uint64_t>(-4), 8));
  Block.Ops.push_back(FormAddr);

  NdVar Value = NdVar::tmp(TmpBase + TmpStride, 8);
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = Value;
  Load.addInput(Addr);
  Block.Ops.push_back(Load);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.addInput(Value);
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 4);
  EXPECT_EQ(Med.FrameHeadroom, 4);
}

TEST_F(COFFARMPipeline, StackAnalysisKillsAddressOnArbitraryRedefinition) {
  constexpr Arch TheArch = Arch::ARM;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "reused_frame_scratch";
  LowBlock Block;
  Block.Id = 0;

  NdVar Reused = NdVar::tmp(TmpBase, TRI.PointerSize);
  LowOp FormAddr;
  FormAddr.Opcode = NdOp::INT_SUB;
  FormAddr.Output = Reused;
  FormAddr.addInput(NdVar::reg(TRI.StackPointer, TRI.PointerSize));
  FormAddr.addInput(NdVar::cst(24, TRI.PointerSize));
  Block.Ops.push_back(FormAddr);

  LowOp RedefineAsData;
  RedefineAsData.Opcode = NdOp::INT_LEFT;
  RedefineAsData.Output = Reused;
  RedefineAsData.addInput(
      NdVar::reg(TRI.IntParamRegs.front(), TRI.PointerSize));
  RedefineAsData.addInput(NdVar::cst(24, TRI.PointerSize));
  Block.Ops.push_back(RedefineAsData);

  NdVar DataReg = NdVar::reg(TRI.IntParamRegs[2], TRI.PointerSize);
  LowOp CopyData;
  CopyData.Opcode = NdOp::COPY;
  CopyData.Output = DataReg;
  CopyData.addInput(Reused);
  Block.Ops.push_back(CopyData);

  LowOp ModerateDataAdd;
  ModerateDataAdd.Opcode = NdOp::INT_ADD;
  ModerateDataAdd.Output = DataReg;
  ModerateDataAdd.addInput(DataReg);
  ModerateDataAdd.addInput(NdVar::cst(100000, TRI.PointerSize));
  Block.Ops.push_back(ModerateDataAdd);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.addInput(DataReg);
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 24);
  EXPECT_EQ(Med.FrameHeadroom, 0);
}

TEST_F(COFFARMPipeline, StackAnalysisAccumulatesInPlaceSPUpdates) {
  constexpr Arch TheArch = Arch::X86;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "in_place_sp_updates";
  LowBlock Block;
  Block.Id = 0;
  NdVar SP = NdVar::reg(TRI.StackPointer, TRI.PointerSize);
  for (uint64_t Amount : {4u, 4u, 24u}) {
    LowOp Sub;
    Sub.Opcode = NdOp::INT_SUB;
    Sub.Output = SP;
    Sub.addInput(SP);
    Sub.addInput(NdVar::cst(Amount, TRI.PointerSize));
    Block.Ops.push_back(Sub);
  }
  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 32);
  EXPECT_EQ(Med.FrameHeadroom, 0);
}

TEST_F(COFFARMPipeline, StackAnalysisPropagatesAddressThroughWidthViews) {
  constexpr Arch TheArch = Arch::X86;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "frame_address_width_views";
  LowBlock Block;
  Block.Id = 0;

  NdVar Wide = NdVar::tmp(TmpBase, 8);
  LowOp Extend;
  Extend.Opcode = NdOp::INT_ZEXT;
  Extend.Output = Wide;
  Extend.addInput(NdVar::reg(TRI.StackPointer, TRI.PointerSize));
  Block.Ops.push_back(Extend);

  NdVar WideAddr = NdVar::tmp(TmpBase + TmpStride, 8);
  LowOp FormAddr;
  FormAddr.Opcode = NdOp::INT_SUB;
  FormAddr.Output = WideAddr;
  FormAddr.addInput(Wide);
  FormAddr.addInput(NdVar::cst(8, 8));
  Block.Ops.push_back(FormAddr);

  NdVar NarrowAddr = NdVar::tmp(TmpBase + 2 * TmpStride, TRI.PointerSize);
  LowOp Narrow;
  Narrow.Opcode = NdOp::SUBBYTES;
  Narrow.Output = NarrowAddr;
  Narrow.addInput(WideAddr);
  Narrow.addInput(NdVar::cst(0, 8));
  Block.Ops.push_back(Narrow);

  NdVar Value = NdVar::tmp(TmpBase + 3 * TmpStride, TRI.PointerSize);
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = Value;
  Load.addInput(NarrowAddr);
  Block.Ops.push_back(Load);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.addInput(Value);
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 8);
  EXPECT_EQ(Med.FrameHeadroom, 0);
}

TEST_F(COFFARMPipeline, StackAnalysisRetainsFrameBaseThroughDynamicIndex) {
  constexpr Arch TheArch = Arch::X64;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "red_zone_dynamic_index";
  LowBlock Block;
  Block.Id = 0;

  NdVar Addr = NdVar::tmp(TmpBase, TRI.PointerSize);
  LowOp CopySP;
  CopySP.Opcode = NdOp::COPY;
  CopySP.Output = Addr;
  CopySP.addInput(NdVar::reg(TRI.StackPointer, TRI.PointerSize));
  Block.Ops.push_back(CopySP);

  LowOp AddIndex;
  AddIndex.Opcode = NdOp::INT_ADD;
  AddIndex.Output = Addr;
  AddIndex.addInput(Addr);
  AddIndex.addInput(NdVar::reg(TRI.IntParamRegs.front(), TRI.PointerSize));
  Block.Ops.push_back(AddIndex);

  LowOp AddBound;
  AddBound.Opcode = NdOp::INT_SUB;
  AddBound.Output = Addr;
  AddBound.addInput(Addr);
  AddBound.addInput(NdVar::cst(105, TRI.PointerSize));
  Block.Ops.push_back(AddBound);

  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Addr);
  Store.addInput(NdVar::cst(1, 1));
  Block.Ops.push_back(Store);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 105);
  EXPECT_EQ(Med.FrameHeadroom, 0);
}

TEST_F(COFFARMPipeline, StackAnalysisScopesFlagPairsToInstruction) {
  constexpr Arch TheArch = Arch::X64;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "frame_address_reuses_flag_temporaries";
  LowBlock Block;
  Block.Id = 0;

  NdVar Addr = NdVar::tmp(TmpBase, TRI.PointerSize);
  NdVar Index = NdVar::tmp(TmpBase + TmpStride, TRI.PointerSize);

  LowOp UnrelatedFlag;
  UnrelatedFlag.Opcode = NdOp::INT_CARRY;
  UnrelatedFlag.Addr = 0x1000;
  UnrelatedFlag.Output = NdVar::tmp(TmpBase + 2 * TmpStride, 1);
  UnrelatedFlag.addInput(Addr);
  UnrelatedFlag.addInput(Index);
  Block.Ops.push_back(UnrelatedFlag);

  LowOp CopySP;
  CopySP.Opcode = NdOp::COPY;
  CopySP.Addr = 0x1004;
  CopySP.Output = Addr;
  CopySP.addInput(NdVar::reg(TRI.StackPointer, TRI.PointerSize));
  Block.Ops.push_back(CopySP);

  LowOp CopyIndex;
  CopyIndex.Opcode = NdOp::COPY;
  CopyIndex.Addr = 0x1004;
  CopyIndex.Output = Index;
  CopyIndex.addInput(
      NdVar::reg(TRI.IntParamRegs.front(), TRI.PointerSize));
  Block.Ops.push_back(CopyIndex);

  LowOp AddIndex;
  AddIndex.Opcode = NdOp::INT_ADD;
  AddIndex.Addr = 0x1004;
  AddIndex.Output = Addr;
  AddIndex.addInput(Addr);
  AddIndex.addInput(Index);
  Block.Ops.push_back(AddIndex);

  LowOp AddBound;
  AddBound.Opcode = NdOp::INT_SUB;
  AddBound.Addr = 0x1004;
  AddBound.Output = Addr;
  AddBound.addInput(Addr);
  AddBound.addInput(NdVar::cst(128, TRI.PointerSize));
  Block.Ops.push_back(AddBound);

  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.Addr = 0x1004;
  Store.addInput(Addr);
  Store.addInput(NdVar::cst(1, 1));
  Block.Ops.push_back(Store);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 128);
  EXPECT_EQ(Med.FrameHeadroom, 0);
}

TEST_F(COFFARMPipeline, StackAnalysisRefinesWidthBeforeAddressReuse) {
  constexpr Arch TheArch = Arch::X64;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "frame_width_before_address_reuse";
  LowBlock Block;
  Block.Id = 0;

  NdVar Addr = NdVar::tmp(TmpBase, TRI.PointerSize);
  LowOp FormAddr;
  FormAddr.Opcode = NdOp::INT_SUB;
  FormAddr.Output = Addr;
  FormAddr.addInput(NdVar::reg(TRI.StackPointer, TRI.PointerSize));
  FormAddr.addInput(NdVar::cst(32, TRI.PointerSize));
  Block.Ops.push_back(FormAddr);

  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Addr);
  Store.addInput(NdVar::cst(1, 16));
  Block.Ops.push_back(Store);

  LowOp ReuseAsData;
  ReuseAsData.Opcode = NdOp::INT_XOR;
  ReuseAsData.Output = Addr;
  ReuseAsData.addInput(NdVar::reg(TRI.IntParamRegs.front(), TRI.PointerSize));
  ReuseAsData.addInput(NdVar::cst(0x55, TRI.PointerSize));
  Block.Ops.push_back(ReuseAsData);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 32);
  EXPECT_TRUE(std::any_of(Med.Locals.begin(), Med.Locals.end(),
                          [](const MedVar &Local) {
                            return Local.StackOff == -32 && Local.Size == 16;
                          }));
}

TEST_F(COFFARMPipeline, LLVMStackStorageMergesHeadroomAndPreservesX64Residue) {
  auto MakeVoidFunc = [](llvm::StringRef Name) {
    MedFunc Func;
    Func.Name = Name.str();
    Func.ReturnType = NdType::makeVoid();
    MedBlock Block;
    Block.Id = 0;
    MedOp Ret;
    Ret.Opcode = NdOp::RETURN;
    Block.Ops.push_back(Ret);
    Func.Blocks.push_back(std::move(Block));
    return Func;
  };
  auto EmitIR = [](const MedFunc &Func, Arch TheArch,
                   BinaryFormat Format = BinaryFormat::ELF) {
    llvm::LLVMContext Ctx;
    auto Module = MedLLVMEmitter().emit({Func}, Ctx, "stack_test", TheArch, {},
                                        nullptr, Format);
    EXPECT_NE(Module, nullptr);
    std::string IR;
    llvm::raw_string_ostream OS(IR);
    if (Module)
      Module->print(OS, nullptr);
    OS.flush();
    return IR;
  };

  MedFunc Variadic = MakeVoidFunc("variadic_headroom");
  Variadic.FrameHeadroom = 64;
  Variadic.IsVariadic = true;
  Variadic.VariadicOverflowBase = 0;
  Variadic.VariadicOverflowCount = 1;
  std::string VariadicIR = EmitIR(Variadic, Arch::AArch64);
  EXPECT_NE(VariadicIR.find("alloca [128 x i8]"), std::string::npos)
      << VariadicIR;

  MedFunc NativeVariadic = MakeVoidFunc("native_variadic_overflow");
  NativeVariadic.FrameSize = 16;
  NativeVariadic.FrameHeadroom = 64;
  NativeVariadic.IsVariadic = true;
  std::string NativeVariadicIR = EmitIR(NativeVariadic, Arch::AArch64);
  EXPECT_NE(NativeVariadicIR.find("alloca [16 x i8]"), std::string::npos)
      << NativeVariadicIR;
  EXPECT_EQ(NativeVariadicIR.find("alloca [96 x i8]"), std::string::npos)
      << NativeVariadicIR;

  MedFunc X64 = MakeVoidFunc("x64_entry_residue");
  X64.FrameSize = 4;
  std::string X64IR = EmitIR(X64, Arch::X64);
  EXPECT_NE(X64IR.find("alloca [24 x i8]"), std::string::npos) << X64IR;
  EXPECT_NE(X64IR.find("getelementptr inbounds i8, ptr %frame, i64 24"),
            std::string::npos)
      << X64IR;

  MedFunc I386 = MakeVoidFunc("i386_entry_residue");
  I386.FrameSize = 4;
  std::string MachOIR = EmitIR(I386, Arch::X86, BinaryFormat::MachO);
  EXPECT_NE(MachOIR.find("alloca [28 x i8]"), std::string::npos) << MachOIR;
  EXPECT_NE(MachOIR.find("getelementptr inbounds i8, ptr %frame, i64 28"),
            std::string::npos)
      << MachOIR;

  std::string COFFIR = EmitIR(I386, Arch::X86, BinaryFormat::COFF);
  EXPECT_NE(COFFIR.find("alloca [16 x i8]"), std::string::npos) << COFFIR;
  EXPECT_NE(COFFIR.find("getelementptr inbounds i8, ptr %frame, i64 16"),
            std::string::npos)
      << COFFIR;
}

TEST_F(COFFARMPipeline, HighCX64StackBaseHasNativeEntryResidue) {
  HighFunc Func;
  Func.Name = "x64_stack_residue";
  Func.FrameSize = 4;
  Func.ReturnType = NdType::makeInt(4);

  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.TheArch = Arch::X64;
  SP.Id = 1;
  SP.Size = 8;
  SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  auto Addr = HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeVar(SP), HighExpr::makeConst(4, 8));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeLoad(Addr, NdType::makeInt(4));
  Func.Body.push_back(std::move(Ret));

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
  OS.flush();

  EXPECT_NE(C.find("_Alignas(16) uint8_t stack_storage[24];"),
            std::string::npos)
      << C;
  EXPECT_NE(C.find("(uintptr_t)(stack_storage + 24);"), std::string::npos)
      << C;
}

TEST_F(COFFARMPipeline, HighCI386StackBaseUsesBinaryFormatResidue) {
  auto EmitC = [&](BinaryFormat Format, llvm::StringRef Name) {
    HighFunc Func;
    Func.Name = Name.str();
    Func.FrameSize = 4;
    Func.ReturnType = NdType::makeInt(4);

    MedVar SP;
    SP.Kind = MedVar::Reg;
    SP.TheArch = Arch::X86;
    SP.Id = 1;
    SP.Size = 4;
    SP.RegOff = getTargetRegInfo(Arch::X86).StackPointer;
    auto Addr = HighExpr::makeBinop(
        NdOp::INT_SUB, HighExpr::makeVar(SP), HighExpr::makeConst(4, 4));
    HighStmt Ret;
    Ret.Kind = StmtKind::Return;
    Ret.RetVal = HighExpr::makeLoad(Addr, Func.ReturnType);
    Func.Body.push_back(std::move(Ret));

    std::string C;
    llvm::raw_string_ostream OS(C);
    CEmitterOptions Opts;
    Opts.TheArch = Arch::X86;
    Opts.Format = Format;
    EXPECT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
    OS.flush();
    return C;
  };

  std::string MachO = EmitC(BinaryFormat::MachO, "macho_i386_stack");
  EXPECT_NE(MachO.find("stack_storage[28]"), std::string::npos) << MachO;
  EXPECT_NE(MachO.find("(uintptr_t)(stack_storage + 28)"), std::string::npos)
      << MachO;

  std::string COFF = EmitC(BinaryFormat::COFF, "coff_i386_stack");
  EXPECT_NE(COFF.find("stack_storage[16]"), std::string::npos) << COFF;
  EXPECT_NE(COFF.find("(uintptr_t)(stack_storage + 16)"), std::string::npos)
      << COFF;
}

TEST_F(COFFARMPipeline, HighCLoadLvaluesAndAddressesRemainValidC) {
  HighFunc Func;
  Func.Name = "load_lvalue";
  Func.ReturnType = NdType::makePtr(NdType::makeInt(4));
  Func.Params.push_back({"arg0", NdType::makePtr(NdType::makeInt(4))});

  MedVar Arg;
  Arg.Kind = MedVar::Param;
  Arg.Id = 0;
  Arg.Size = 8;
  Arg.TheArch = Arch::AArch64;
  auto ArgExpr = HighExpr::makeVar(Arg, Func.Params[0].Type);
  auto Loaded = HighExpr::makeLoad(ArgExpr, NdType::makeInt(4));

  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = Loaded;
  Assign.Val = HighExpr::makeConst(7, 4);
  Func.Body.push_back(std::move(Assign));

  auto Address = std::make_shared<HighExpr>();
  Address->Kind = ExprKind::Addr;
  Address->Type = Func.ReturnType;
  Address->Operands.push_back(Loaded);
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = Address;
  Func.Body.push_back(std::move(Ret));

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
  OS.flush();

  EXPECT_NE(C.find("neverd_mem_store_"), std::string::npos) << C;
  EXPECT_NE(C.find("(int32_t *)(uintptr_t)(arg0)"), std::string::npos) << C;
  EXPECT_EQ(C.find("&neverd_mem_load_"), std::string::npos) << C;

  const fs::path CPath = tmpFile("load_lvalue.c");
  std::ofstream Out(CPath);
  Out << C;
  Out.close();
  ASSERT_TRUE(Out.good());
  RunResult Syntax = exec("clang", {"-std=c11", "-fsyntax-only",
                                    CPath.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
}

TEST_F(COFFARMPipeline, HighCForwardedParameterReturnDoesNotBecomeVoid) {
  HighFunc Func;
  Func.Name = "forwarded_parameter";
  Func.ReturnType = NdType::makeInt(4);
  Func.Params.push_back({"arg0", NdType::makeInt(4)});
  Func.Params.push_back({"arg1", NdType::makePtr(NdType::makeInt(4))});

  MedVar Value;
  Value.Kind = MedVar::Param;
  Value.Id = 0;
  Value.Size = 4;
  Value.TheArch = Arch::AArch64;
  MedVar Address = Value;
  Address.Id = 1;
  Address.Size = 8;

  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeVar(Address, Func.Params[1].Type);
  Store.StoreVal = HighExpr::makeVar(Value, Func.Params[0].Type);
  Func.Body.push_back(std::move(Store));

  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeLoad(
      HighExpr::makeVar(Address, Func.Params[1].Type), Func.ReturnType);
  Func.Body.push_back(std::move(Ret));

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
  OS.flush();

  auto Body = cFunctionBody(C, "forwarded_parameter");
  ASSERT_TRUE(Body.has_value()) << C;
  EXPECT_NE(Body->find("return arg0;"), std::string::npos) << *Body;
  EXPECT_EQ(C.find("void forwarded_parameter("), std::string::npos) << C;

  const fs::path CPath = tmpFile("forwarded_parameter.c");
  std::ofstream Out(CPath);
  Out << C;
  Out.close();
  ASSERT_TRUE(Out.good());
  RunResult Syntax =
      exec("clang", {"-std=c11", "-fsyntax-only", CPath.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
}

TEST_F(COFFARMPipeline, HighCForwardingPreservesFrameLvaluesAndAddresses) {
  HighFunc Func;
  Func.Name = "frame_lvalue_address";
  Func.FrameSize = 4;
  Func.ReturnType = NdType::makePtr(NdType::makeInt(4));

  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.TheArch = Arch::AArch64;
  SP.Id = 1;
  SP.Size = 8;
  SP.RegOff = getTargetRegInfo(Arch::AArch64).StackPointer;
  auto FrameAddr = [&]() {
    return HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeVar(SP),
                               HighExpr::makeConst(4, 8));
  };

  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = FrameAddr();
  Store.StoreVal = HighExpr::makeConst(7, 4);
  Func.Body.push_back(std::move(Store));

  HighStmt Read;
  Read.Kind = StmtKind::ExprStmt;
  Read.Val = HighExpr::makeLoad(FrameAddr(), NdType::makeInt(4));
  Func.Body.push_back(std::move(Read));

  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = HighExpr::makeLoad(FrameAddr(), NdType::makeInt(4));
  Assign.Val = HighExpr::makeConst(9, 4);
  Func.Body.push_back(std::move(Assign));

  auto Address = std::make_shared<HighExpr>();
  Address->Kind = ExprKind::Addr;
  Address->Type = Func.ReturnType;
  Address->Operands.push_back(
      HighExpr::makeLoad(FrameAddr(), NdType::makeInt(4)));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = Address;
  Func.Body.push_back(std::move(Ret));

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
  OS.flush();

  auto Body = cFunctionBody(C, "frame_lvalue_address");
  ASSERT_TRUE(Body.has_value()) << C;
  EXPECT_NE(Body->find("stack_storage[16]"), std::string::npos) << *Body;
  EXPECT_NE(Body->find("frame_base - 4"), std::string::npos) << *Body;

  const fs::path CPath = tmpFile("frame_lvalue_address.c");
  std::ofstream Out(CPath);
  Out << C;
  Out.close();
  ASSERT_TRUE(Out.good());
  RunResult Syntax =
      exec("clang", {"-std=c11", "-fsyntax-only", CPath.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
}

TEST_F(COFFARMPipeline, HighCAddressOfLoadDoesNotDeleteStore) {
  HighFunc Func;
  Func.Name = "address_preserves_store";
  Func.ReturnType = NdType::makePtr(NdType::makeInt(4));
  Func.Params.push_back({"arg0", NdType::makePtr(NdType::makeInt(4))});

  MedVar Address;
  Address.Kind = MedVar::Param;
  Address.Id = 0;
  Address.Size = 8;
  Address.TheArch = Arch::AArch64;
  auto ParamAddress = [&]() {
    return HighExpr::makeVar(Address, Func.Params[0].Type);
  };

  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = ParamAddress();
  Store.StoreVal = HighExpr::makeConst(7, 4);
  Func.Body.push_back(std::move(Store));

  auto AddressOf = std::make_shared<HighExpr>();
  AddressOf->Kind = ExprKind::Addr;
  AddressOf->Type = Func.ReturnType;
  AddressOf->Operands.push_back(
      HighExpr::makeLoad(ParamAddress(), NdType::makeInt(4)));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = AddressOf;
  Func.Body.push_back(std::move(Ret));

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
  OS.flush();

  auto Body = cFunctionBody(C, "address_preserves_store");
  ASSERT_TRUE(Body.has_value()) << C;
  EXPECT_NE(Body->find("neverd_mem_store_"), std::string::npos) << *Body;
  EXPECT_NE(Body->find(", 7);"), std::string::npos) << *Body;
  EXPECT_NE(Body->find("return (int32_t *)(uintptr_t)(arg0);"),
            std::string::npos)
      << *Body;

  const fs::path CPath = tmpFile("address_preserves_store.c");
  std::ofstream Out(CPath);
  Out << C;
  Out.close();
  ASSERT_TRUE(Out.good());
  RunResult Syntax =
      exec("clang", {"-std=c11", "-fsyntax-only", CPath.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
}

} // namespace
