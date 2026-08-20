//===- SinkCatalogTests.cpp - Catalog normalization and matching ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/safety/SinkCatalog.h"

#include "llvm/Support/raw_ostream.h"

#include <fstream>

using namespace neverd;
using namespace neverd::safety;

TEST(SinkCatalog, NormalizesLeadingUnderscores) {
  EXPECT_EQ(SinkCatalog::normalize("_malloc"), "malloc");
  EXPECT_EQ(SinkCatalog::normalize("___strcpy_chk"), "strcpy_chk");
  EXPECT_EQ(SinkCatalog::normalize("memcpy"), "memcpy");
  EXPECT_EQ(SinkCatalog::normalize("__imp_memcpy"), "memcpy");
  EXPECT_EQ(SinkCatalog::normalize("msvcrt.dll!memcpy"), "memcpy");
}

TEST(SinkCatalog, MatchesImportThunkSpelling) {
  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_NE(C.matchSink("__imp_memcpy"), nullptr);
  EXPECT_EQ(C.matchSink("__imp_memcpy"), C.matchSink("memcpy"));
}

TEST(SinkCatalog, MatchesAcrossFormatSpellings) {
  SinkCatalog C = SinkCatalog::defaults();
  // ELF/PE spelling and the Mach-O underscore-prefixed spelling fold together.
  const SinkEntry *A = C.matchSink("memcpy");
  const SinkEntry *B = C.matchSink("_memcpy");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  EXPECT_EQ(A, B);
  EXPECT_EQ(A->Kind, SinkKind::Copy);
  EXPECT_EQ(A->Class, VulnClass::BufferOverflow);
}

TEST(SinkCatalog, MatchesItaniumDemangledMemcpy) {
  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_NE(C.matchSink("_Z6memcpyPvS_m"), nullptr);
  EXPECT_EQ(C.matchSink("_Z6memcpyPvS_m"), C.matchSink("memcpy"));
}

TEST(SinkCatalog, MatchesAllocaAliases) {
  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_NE(C.matchSink("alloca"), nullptr);
  EXPECT_EQ(C.matchSink("alloca")->Kind, SinkKind::StackAlloc);
  EXPECT_EQ(C.matchSink("__builtin_alloca"), C.matchSink("alloca"));
}

TEST(SinkCatalog, CopyArgumentLayout) {
  SinkCatalog C = SinkCatalog::defaults();
  // memcpy(dst, src, n): the count decides the write.
  const SinkEntry *M = C.matchSink("memcpy");
  ASSERT_NE(M, nullptr);
  EXPECT_EQ(M->DstArg, 0);
  EXPECT_EQ(M->SrcArg, 1);
  EXPECT_EQ(M->LenArg, 2);
  EXPECT_EQ(M->decidingArg(), 2);

  // strcpy(dst, src): no explicit count, so the source length decides.
  const SinkEntry *S = C.matchSink("strcpy");
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->DstArg, 0);
  EXPECT_EQ(S->SrcArg, 1);
  EXPECT_EQ(S->LenArg, -1);
  EXPECT_EQ(S->decidingArg(), 1);
}

TEST(SinkCatalog, FortifiedVariantCarriesCapacity) {
  SinkCatalog C = SinkCatalog::defaults();
  // The `___strcpy_chk` Mach-O spelling matches and exposes its capacity slot.
  const SinkEntry *S = C.matchSink("___strcpy_chk");
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->CapArg, 2);
  // A fortified variant is a distinct entry, not folded onto plain strcpy.
  EXPECT_EQ(C.matchSink("strcpy")->CapArg, -1);
}

TEST(SinkCatalog, HeapAllocAndFree) {
  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_NE(C.matchSink("malloc"), nullptr);
  EXPECT_EQ(C.matchSink("malloc")->Kind, SinkKind::Alloc);
  EXPECT_EQ(C.matchSink("free")->Kind, SinkKind::Free);
  EXPECT_EQ(C.matchSink("realloc")->Kind, SinkKind::Realloc);
  EXPECT_EQ(C.matchSink("reallocf")->Kind, SinkKind::Realloc);
}

TEST(SinkCatalog, MangledOperatorNewAndDelete) {
  SinkCatalog C = SinkCatalog::defaults();
  // Itanium operator new / delete, in their platform underscore spellings.
  ASSERT_NE(C.matchSink("_Znwm"), nullptr);
  EXPECT_EQ(C.matchSink("_Znwm")->Kind, SinkKind::Alloc);
  ASSERT_NE(C.matchSink("_ZdlPv"), nullptr);
  EXPECT_EQ(C.matchSink("_ZdlPv")->Kind, SinkKind::Free);
}

TEST(SinkCatalog, DefaultSourcesCoverPosixAndWin32) {
  SinkCatalog C = SinkCatalog::defaults();
  EXPECT_NE(C.matchSource("getenv"), nullptr);
  EXPECT_NE(C.matchSource("recv"), nullptr);
  // A PE hunt must recognize Win32 input providers, not only POSIX ones.
  EXPECT_NE(C.matchSource("GetCommandLineA"), nullptr);
  const SourceEntry *Read = C.matchSource("read");
  ASSERT_NE(Read, nullptr);
  EXPECT_EQ(Read->OutArg, 1); // read(fd, buf, n) taints buf.
  // PE/ucrt spells the same source as `_read`; the leading underscore folds.
  EXPECT_EQ(C.matchSource("_read"), Read);
}

TEST(SafetyVocab, JsonSpellingsComeFromTheEnumTable) {
  EXPECT_STREQ(toString(Track::Hunt), "hunt");
  EXPECT_STREQ(toString(Track::Audit), "audit");
  EXPECT_STREQ(toString(Verdict::Unsafe), "UNSAFE");
  EXPECT_STREQ(toString(SinkKind::Copy), "copy");
  EXPECT_STREQ(toString(VulnClass::HeapLeak), "heap_leak");
  EXPECT_STREQ(toString(NameSource::Import), "import");
  EXPECT_STREQ(toString(ArgFlow::Tainted), "TAINTED");
}

TEST(SinkCatalog, FileOverrideReplacesEntry) {
  std::string Path = std::string(::testing::TempDir()) + "/neverd_sinks.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"my_copy","kind":"copy","dst":0,"src":1,)"
       << R"("len":2,"severity":90}]})";
  }
  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_FALSE((bool)C.mergeSinksFromFile(Path));
  const SinkEntry *E = C.matchSink("my_copy");
  ASSERT_NE(E, nullptr);
  EXPECT_EQ(E->LenArg, 2);
  EXPECT_EQ(E->Severity, 90u);
}
