//===- PythonRecoveryTests.cpp - Python artifact qualification ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the shared reader with owned compiler artifacts and independent
/// mutations of physical offsets, framing and exception cleanup.
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/bytecode/python/Artifacts.h"
#include "gtest/gtest.h"

#include "neverd/analysis/PythonRecovery.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Base64.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>

using namespace llvm;
using namespace neverd::python;
namespace {
const json::Array &fixtures() {
  static const auto Rows = [] {
    auto File = MemoryBuffer::getFile(PYTHON_ARTIFACT_MANIFEST);
    if (!File)
      return json::Array{};
    auto R = json::parse((*File)->getBuffer());
    if (!R) {
      consumeError(R.takeError());
      return json::Array{};
    }
    return std::move(*R->getAsObject()->getArray("fixtures"));
  }();
  return Rows;
}
std::string bytes(StringRef Name) {
  for (const auto &V : fixtures()) {
    const auto &R = *V.getAsObject();
    if (R.getString("name") != Name)
      continue;
    std::vector<char> Data;
    if (auto E = decodeBase64(*R.getString("data"), Data)) {
      consumeError(std::move(E));
      return {};
    }
    return {Data.begin(), Data.end()};
  }
  return {};
}
json::Object analyze(StringRef Bytes, StringRef Name = "input.pyc",
                     int Component = -1) {
  return recover(Bytes, Name, "auto", [] { return false; }, Component);
}
const json::Object *named(const json::Array &Rows, StringRef Name) {
  for (const auto &V : Rows)
    if (V.getAsObject()->getString("name") == Name)
      return V.getAsObject();
  return nullptr;
}
TEST(PythonRecovery, OwnedCodeObjectsMatchDisassembly) {
  unsigned Cases = 0;
  for (const auto &Value : fixtures()) {
    const auto &Fixture = *Value.getAsObject();
    const auto *Oracle = Fixture.getArray("disassembly");
    if (!Oracle)
      continue;
    ++Cases;
    auto Name = *Fixture.getString("name");
    SCOPED_TRACE(Name.str());
    auto Data = bytes(Name);
    auto R = analyze(Data);
    ASSERT_EQ(R.getString("status"), "reconstructed_source");
    EXPECT_EQ(R.getString("input_sha256"), Fixture.getString("sha256"));
    const auto &Component = *R.getArray("components")->front().getAsObject();
    EXPECT_EQ(Component.getString("header_mode"), Fixture.getString("header"));
    const auto &Functions = *R.getArray("functions");
    ASSERT_EQ(Functions.size(), Oracle->size());
    for (size_t N = 0; N < Functions.size(); ++N) {
      const auto &F = *Functions[N].getAsObject(),
                 &Expected = *(*Oracle)[N].getAsObject();
      EXPECT_EQ(F.getString("name"), Expected.getString("name"));
      ASSERT_TRUE(F.getString("source"));
      const auto &Code = *F.getArray("instructions"),
                 &Instructions = *Expected.getArray("instructions");
      ASSERT_EQ(Code.size(), Instructions.size());
      for (size_t I = 0; I < Code.size(); ++I) {
        const auto &Row = *Code[I].getAsObject();
        const auto &E = *Instructions[I].getAsArray();
        EXPECT_EQ(Row.getInteger("pc"), E[0].getAsInteger());
        EXPECT_EQ(Row.getString("opcode"), E[1].getAsString());
        EXPECT_EQ(Row.getInteger("original_line"), E[2].getAsInteger());
        EXPECT_EQ(Row.getInteger("target"), E[3].getAsInteger());
        EXPECT_EQ(*Row.getInteger("offset") - *F.getInteger("bytecode_offset"),
                  *Row.getInteger("pc"));
        if (auto Line = Row.getInteger("source_line"))
          EXPECT_LE(*Line, F.getString("source")->count('\n'));
      }
    }
  }
  EXPECT_EQ(Cases, 4u);
}
TEST(PythonRecovery, SourceBytesHeadersCancellationAndBudget) {
  std::string Source = "\xef\xbb\xbf# owned\r\nreturn_value = 3\r\n";
  auto R = analyze(Source, "owned.py");
  EXPECT_EQ(R.getString("status"), "extracted_source");
  EXPECT_EQ(R.getArray("functions")->front().getAsObject()->getString("source"),
            Source);
  Source = "# coding: latin-1\r\ns = '\xe9'\r\n";
  auto Latin = analyze(Source, "owned.py");
  const auto &F = *Latin.getArray("functions")->front().getAsObject();
  EXPECT_EQ(F.getString("source_base64"), encodeBase64(Source));
  EXPECT_TRUE(F.getString("source")->contains("\xc3\xa9"));
  auto Data = bytes("program0.pyc");
  ASSERT_GT(Data.size(), 16u);
  EXPECT_EQ(recover(Data, "owned.pyc", "unknown", [] { return false; })
                .getString("status"),
            "unsupported");
  unsigned Checks = 0;
  EXPECT_EQ(recover(Data, "owned.pyc", "auto", [&] { return ++Checks == 12; })
                .getString("status"),
            "cancelled");
  auto Bad = Data;
  Bad[0] = char(0xa7);
  EXPECT_EQ(analyze(Bad).getString("status"), "unsupported");
  Bad = Data;
  Bad[4] = 2;
  EXPECT_EQ(analyze(Bad).getString("status"), "malformed");
  Bad = Data;
  Bad[16] = 'r';
  support::endian::write32le(Bad.data() + 17, 70000);
  EXPECT_EQ(analyze(Bad).getString("status"), "malformed");
  auto Small = bytes("optimization2.pyc");
  for (size_t N = 0; N < Small.size(); ++N) {
    auto Partial = analyze(StringRef(Small).take_front(N));
    ASSERT_NE(Partial.getString("status"), "reconstructed_source") << N;
  }
  EXPECT_EQ(analyze(Data + "x").getString("status"), "malformed");
  EXPECT_EQ(analyze(std::string(limits::kTextBytes + 1, 'x'), "owned.py")
                .getString("status"),
            "budget_exceeded");
  auto Large = analyze("x = 1\n", "owned.py");
  (*Large.getArray("functions")->front().getAsObject())["source"] =
      std::string(limits::kReportBytes, 'x');
  auto Bounded = serializeReport(std::move(Large));
  EXPECT_LT(Bounded.size(), limits::kReportBytes);
  auto Summary = json::parse(Bounded);
  ASSERT_TRUE(static_cast<bool>(Summary));
  EXPECT_EQ(Summary->getAsObject()->getString("status"), "budget_exceeded");
  EXPECT_TRUE(Summary->getAsObject()->getString("input_sha256"));
}
TEST(PythonRecovery, PartialFunctionPreservesSiblingsAndRejectsAlteredCleanup) {
  auto Data = bytes("program0.pyc");
  auto Good = analyze(Data);
  const auto &Functions = *Good.getArray("functions");
  auto *Choose = named(Functions, "choose");
  ASSERT_NE(Choose, nullptr);
  Data[*Choose->getInteger("bytecode_offset")] = char(255);
  auto R = analyze(Data);
  EXPECT_EQ(R.getString("status"), "partial_reconstruction");
  auto &Rows = *R.getArray("functions");
  auto *Failed = named(Rows, "choose");
  ASSERT_NE(Failed, nullptr);
  EXPECT_FALSE(Failed->getString("source"));
  EXPECT_FALSE(*Failed->getBoolean("decode_complete"));
  EXPECT_EQ(Failed->getArray("instructions")
                ->front()
                .getAsObject()
                ->getString("opcode"),
            "unknown");
  EXPECT_EQ(named(Rows, "arithmetic")->getString("status"),
            "reconstructed_source");
  Data = bytes("program0.pyc");
  auto *Using = named(Functions, "using");
  ASSERT_NE(Using, nullptr);
  bool Changed = false;
  for (const auto &V : *Using->getArray("instructions")) {
    auto &I = *V.getAsObject();
    if (I.getString("opcode") == "RERAISE") {
      Data[*I.getInteger("offset") + 1] = 0;
      Changed = true;
      break;
    }
  }
  ASSERT_TRUE(Changed);
  R = analyze(Data);
  EXPECT_FALSE(named(*R.getArray("functions"), "using")->getString("source"));
  Data = bytes("program0.pyc");
  auto *Guarded = named(Functions, "guarded");
  ASSERT_NE(Guarded, nullptr);
  bool ChangedCall = false;
  for (const auto &V : *Guarded->getArray("instructions")) {
    auto &I = *V.getAsObject();
    if (I.getString("opcode") == "CALL_METHOD") {
      Data[*I.getInteger("offset")] = static_cast<char>(Op::CALL_FUNCTION);
      ChangedCall = true;
    }
  }
  ASSERT_TRUE(ChangedCall);
  R = analyze(Data);
  EXPECT_FALSE(named(*R.getArray("functions"), "guarded")->getString("source"));
}
TEST(PythonRecovery, PackageMembersHaveDistinctIdentitiesAndNativeRoutes) {
  auto Data = bytes("owned.pkg");
  ASSERT_FALSE(Data.empty());
  auto R = analyze(Data, "owned.pkg");
  EXPECT_EQ(R.getString("status"), "reconstructed_source");
  const auto &Members = *R.getArray("components");
  auto *Entry = named(Members, "entry");
  auto *Child = named(Members, "owned.child");
  auto *Native = named(Members, "owned.so");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Child, nullptr);
  ASSERT_NE(Native, nullptr);
  EXPECT_EQ(R.getInteger("root"), Entry->getInteger("root"));
  EXPECT_EQ(Child->getString("status"), "available");
  EXPECT_FALSE(Child->getInteger("stored_file_offset"));
  EXPECT_TRUE(Child->getInteger("stored_offset"));
  EXPECT_TRUE(Entry->getInteger("stored_file_offset"));
  EXPECT_EQ(Native->getString("backend"), "native");
  EXPECT_EQ(Native->getString("producer_evidence"), "Cython symbol family");
  EXPECT_EQ(Native->getString("sha256"), hash(bytes("owned.so")));
  auto Selected = analyze(Data, "owned.pkg", *Child->getInteger("id"));
  EXPECT_EQ(Selected.getString("status"), "reconstructed_source");
  for (const auto &V : *Selected.getArray("functions"))
    EXPECT_EQ(V.getAsObject()->getInteger("component"),
              Child->getInteger("id"));
  auto Missing = analyze(bytes("external.pkg"), "external.pkg");
  EXPECT_EQ(Missing.getString("status"), "partial_reconstruction");
  EXPECT_EQ(
      Missing.getArray("components")->back().getAsObject()->getString("status"),
      "missing_member");
}
TEST(PythonRecovery, InvalidArchiveExtentsNamesAndExpansion) {
  auto Data = bytes("owned.pkg");
  ASSERT_GT(Data.size(), 88u);
  size_t Cookie = Data.size() - 88,
         T = support::endian::read32be(Data.data() + Cookie + 12);
  auto Size = [&](size_t N) {
    return support::endian::read32be(Data.data() + N);
  };
  size_t Second = T + Size(T);
  auto Bad = Data;
  Bad.replace(T + 18, 5, "../xx");
  auto R = analyze(Bad, "owned.pkg");
  EXPECT_EQ(R.getString("status"), "malformed");
  EXPECT_EQ(R.getArray("components")->size(), 1u);
  Bad = Data;
  support::endian::write32be(Bad.data() + Second + 4, Size(T + 4));
  EXPECT_EQ(analyze(Bad, "owned.pkg").getString("status"), "malformed");
  Bad = Data;
  support::endian::write32be(Bad.data() + T + 12, limits::kMemberBytes + 1);
  EXPECT_EQ(analyze(Bad, "owned.pkg").getString("status"), "budget_exceeded");
  Bad = Data;
  support::endian::write32be(Bad.data() + T + 12, 1);
  EXPECT_EQ(analyze(Bad, "owned.pkg").getString("status"), "malformed");
  Bad = Data;
  Bad.pop_back();
  EXPECT_EQ(analyze(Bad, "owned.pkg").getString("status"), "malformed");
  auto PYZ = bytes("members.pyz");
  support::endian::write32be(PYZ.data() + 8, PYZ.size());
  EXPECT_EQ(analyze(PYZ, "members.pyz").getString("status"), "malformed");
}
TEST(PythonRecovery, ModernDiskCachesLinesAndUnwindRegionsMatchCompiler) {
  auto Path = std::filesystem::path(PYTHON_ARTIFACT_MANIFEST).parent_path() /
              "modern.json";
  auto File = MemoryBuffer::getFile(Path.string());
  ASSERT_TRUE(static_cast<bool>(File));
  auto Doc = json::parse((*File)->getBuffer());
  ASSERT_TRUE(static_cast<bool>(Doc));
  const auto &Oracle = *Doc->getAsObject();
  std::vector<char> Raw;
  ASSERT_FALSE(static_cast<bool>(decodeBase64(*Oracle.getString("data"), Raw)));
  std::string Data(Raw.begin(), Raw.end());
  auto Report = analyze(Data);
  ASSERT_EQ(Report.getString("status"), "disassembly_only");
  EXPECT_EQ(Report.getString("input_sha256"), Oracle.getString("sha256"));
  const auto &Functions = *Report.getArray("functions"),
             &Expected = *Oracle.getArray("functions");
  ASSERT_EQ(Functions.size(), Expected.size());
  std::optional<size_t> Cache, Exception, BinaryOperand;
  for (size_t I = 0; I < Functions.size(); ++I) {
    const auto &F = *Functions[I].getAsObject(),
               &E = *Expected[I].getAsObject();
    EXPECT_EQ(F.getString("qualified_name"), E.getString("name"));
    EXPECT_EQ(F.getString("status"), "disassembly_only");
    EXPECT_FALSE(F.get("source"));
    const auto &Instructions = *F.getArray("instructions"),
               &Want = *E.getArray("instructions");
    ASSERT_EQ(Instructions.size(), Want.size());
    for (size_t K = 0; K < Want.size(); ++K) {
      const auto &R = *Instructions[K].getAsObject();
      const auto &W = *Want[K].getAsArray();
      EXPECT_EQ(R.getInteger("pc"), W[0].getAsInteger());
      EXPECT_EQ(R.getString("opcode"), W[1].getAsString());
      EXPECT_EQ(R.getInteger("bytes"), W[2].getAsInteger());
      EXPECT_EQ(R.getInteger("original_line"), W[3].getAsInteger());
      EXPECT_EQ(R.getInteger("target"), W[4].getAsInteger());
      if (R.getString("opcode") == "BINARY_OP")
        BinaryOperand =
            *F.getInteger("bytecode_offset") + *R.getInteger("opcode_pc") + 1;
      if (*R.getInteger("cache_units"))
        Cache =
            *F.getInteger("bytecode_offset") + *R.getInteger("opcode_pc") + 2;
    }
    const auto &Regions = *F.getArray("exception_regions"),
               &ExpectedRegions = *E.getArray("regions");
    ASSERT_EQ(Regions.size(), ExpectedRegions.size());
    for (size_t K = 0; K < Regions.size(); ++K) {
      const auto &R = *Regions[K].getAsObject();
      const auto &W = *ExpectedRegions[K].getAsArray();
      EXPECT_EQ(R.getInteger("start"), W[0].getAsInteger());
      EXPECT_EQ(R.getInteger("end"), W[1].getAsInteger());
      EXPECT_EQ(R.getInteger("target"), W[2].getAsInteger());
      EXPECT_EQ(R.getInteger("depth"), W[3].getAsInteger());
      EXPECT_EQ(R.getBoolean("lasti"), W[4].getAsBoolean());
      Exception = *R.getInteger("offset");
    }
  }
  ASSERT_TRUE(Cache);
  ASSERT_TRUE(Exception);
  auto Bad = Data;
  ASSERT_TRUE(BinaryOperand);
  Bad[*BinaryOperand] = char(255);
  EXPECT_EQ(analyze(Bad).getString("status"), "malformed");
  Bad = Data;
  Bad[*Cache] = 1;
  EXPECT_EQ(analyze(Bad).getString("status"), "partial_reconstruction");
  Bad = Data;
  Bad[*Exception] &= 0x7f;
  EXPECT_EQ(analyze(Bad).getString("status"), "malformed");
  EXPECT_EQ(recover(Data, "modern.pyc", "cpython-3.10", [] { return false; })
                .getString("status"),
            "unsupported");
  EXPECT_EQ(recover(StringRef(Data).drop_front(16), "modern.marshal",
                    "cpython-3.11", [] { return false; })
                .getString("status"),
            "disassembly_only");
}

TEST(PythonRecovery, OnedirAndNativeExportRevalidateSelectedBytes) {
  SmallString<128> Dir;
  ASSERT_FALSE(sys::fs::createUniqueDirectory("neverd-python", Dir));
  auto Cleanup = scope_exit([&] { sys::fs::remove_directories(Dir); });
  auto Path = [&](StringRef N) { return (Dir + "/" + N).str(); };
  auto Write = [&](StringRef N, StringRef Data) {
    std::error_code EC;
    raw_fd_ostream OS(Path(N), EC);
    if (EC)
      return false;
    OS << Data;
    return true;
  };
  auto Pack = bytes("owned.pkg");
  ASSERT_TRUE(Write("bundle", Pack));
  auto R = recoverFile(Dir, "renpy", [] { return false; });
  EXPECT_EQ(R.getArray("components")->front().getAsObject()->getString("kind"),
            "pyinstaller_onedir");
  auto *Native = named(*R.getArray("components"), "owned.so");
  ASSERT_NE(Native, nullptr);
  auto Id = *Native->getInteger("id");
  auto Hash = *Native->getString("sha256");
  EXPECT_EQ(exportNative(Dir, "renpy", Id, "changed", Path("out.o"),
                         [] { return false; })
                .getString("status"),
            "error");
  EXPECT_EQ(
      exportNative(Dir, "renpy", Id, Hash, Path("out.o"), [] { return false; })
          .getString("status"),
      "exported");
  auto File = MemoryBuffer::getFile(Path("out.o"));
  ASSERT_TRUE(static_cast<bool>(File));
  EXPECT_EQ((*File)->getBuffer(), bytes("owned.so"));
  EXPECT_EQ(
      exportNative(Dir, "renpy", Id, Hash, Path("out.o"), [] { return false; })
          .getString("status"),
      "io_error");
  auto Original = MemoryBuffer::getFile(Path("bundle"));
  ASSERT_TRUE(static_cast<bool>(Original));
  EXPECT_EQ((*Original)->getBuffer(), Pack);

  auto External = bytes("external.pkg");
  auto At = External.find("companion:library.bin");
  ASSERT_NE(At, std::string::npos);
  External.replace(At, 21, "companion:members.pyz");
  ASSERT_TRUE(Write("request.pkg", External));
  auto Missing = recoverFile(Dir, "renpy", [] { return false; });
  const auto *Dependency =
      named(*Missing.getArray("components"), "companion:members.pyz");
  ASSERT_NE(Dependency, nullptr);
  EXPECT_EQ(Dependency->getString("status"), "missing_member");
  ASSERT_TRUE(Write("companion", Pack));
  auto Resolved = recoverFile(Dir, "renpy", [] { return false; });
  const auto &Members = *Resolved.getArray("components");
  Dependency = named(Members, "companion:members.pyz");
  ASSERT_NE(Dependency, nullptr);
  ASSERT_TRUE(Dependency->getInteger("resolved_component"));
  auto &Target =
      *Members[*Dependency->getInteger("resolved_component")].getAsObject();
  EXPECT_EQ(Target.getString("name"), "members.pyz");
  EXPECT_EQ(
      Members[*Target.getInteger("parent")].getAsObject()->getString("name"),
      "companion");
}
} // namespace
