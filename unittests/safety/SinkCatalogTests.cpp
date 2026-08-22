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
  EXPECT_EQ(SinkCatalog::normalize("__imp__ReadFile@20"), "ReadFile");
  EXPECT_EQ(SinkCatalog::normalize("KERNEL32.dll!_ReadFile@20"), "ReadFile");
  EXPECT_EQ(SinkCatalog::normalize("__imp_@ReadFile@20"), "ReadFile");
  EXPECT_EQ(SinkCatalog::normalize("memcpy@@24"), "memcpy");
  EXPECT_EQ(SinkCatalog::normalize("name@not_a_size"), "name@not_a_size");
  EXPECT_EQ(SinkCatalog::normalize("@pascal_name"), "@pascal_name");
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

TEST(SinkCatalog, UnboundedWritesRequireAnExplicitSummary) {
  SinkCatalog C = SinkCatalog::defaults();
  const SinkEntry *Gets = C.matchSink("gets");
  ASSERT_NE(Gets, nullptr);
  EXPECT_TRUE(Gets->UnboundedWrite);
  EXPECT_FALSE(C.matchSink("strcpy")->UnboundedWrite);

  std::string Path =
      std::string(::testing::TempDir()) + "/neverd_unbounded_sink.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"custom_input","kind":"copy",)"
       << R"("dst":0,"unbounded":true}]})";
  }
  ASSERT_FALSE((bool)C.mergeSinksFromFile(Path));
  const SinkEntry *Custom = C.matchSink("custom_input");
  ASSERT_NE(Custom, nullptr);
  EXPECT_TRUE(Custom->UnboundedWrite);
}

TEST(SinkCatalog, RejectsInvalidUnboundedSummaryTransactionally) {
  std::string Path =
      std::string(::testing::TempDir()) + "/neverd_bad_unbounded_sink.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"would_publish","kind":"copy","dst":0},)"
       << R"({"name":"bad","kind":"copy","dst":0,"src":1,)"
       << R"("unbounded":true}]})";
  }
  SinkCatalog C = SinkCatalog::defaults();
  llvm::Error Err = C.mergeSinksFromFile(Path);
  ASSERT_TRUE(static_cast<bool>(Err));
  EXPECT_NE(llvm::toString(std::move(Err)).find("destination-only"),
            std::string::npos);
  EXPECT_EQ(C.matchSink("would_publish"), nullptr);
}

TEST(SinkCatalog, FormatArgumentLayoutSeparatesWriteLimitAndObjectCapacity) {
  SinkCatalog C = SinkCatalog::defaults();

  const SinkEntry *Snprintf = C.matchSink("snprintf");
  ASSERT_NE(Snprintf, nullptr);
  EXPECT_EQ(Snprintf->DstArg, 0);
  EXPECT_EQ(Snprintf->LenArg, 1);
  EXPECT_EQ(Snprintf->FmtArg, 2);
  EXPECT_EQ(Snprintf->CapArg, -1);
  EXPECT_EQ(Snprintf->decidingArg(), 2);

  const SinkEntry *Checked = C.matchSink("__snprintf_chk");
  ASSERT_NE(Checked, nullptr);
  EXPECT_EQ(Checked->DstArg, 0);
  EXPECT_EQ(Checked->LenArg, 1);
  EXPECT_EQ(Checked->CapArg, 3);
  EXPECT_EQ(Checked->FmtArg, 4);
  EXPECT_EQ(Checked->decidingArg(), 4);
}

TEST(SinkCatalog, HeapAllocAndFree) {
  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_NE(C.matchSink("malloc"), nullptr);
  EXPECT_EQ(C.matchSink("malloc")->Kind, SinkKind::Alloc);
  EXPECT_EQ(C.matchSink("free")->Kind, SinkKind::Free);
  EXPECT_EQ(C.matchSink("realloc")->Kind, SinkKind::Realloc);
  EXPECT_EQ(C.matchSink("reallocf")->Kind, SinkKind::Realloc);
  EXPECT_FALSE(C.matchSink("free")->ReleaseMayFail);
  for (const char *Name : {"HeapFree", "LocalFree", "GlobalFree"}) {
    SCOPED_TRACE(Name);
    const SinkEntry *Release = C.matchSink(Name);
    ASSERT_NE(Release, nullptr);
    EXPECT_EQ(Release->Kind, SinkKind::Free);
    EXPECT_TRUE(Release->ReleaseMayFail);
  }
  ASSERT_NE(C.matchSink("wcsdup"), nullptr);
  EXPECT_EQ(C.matchSink("wcsdup")->Kind, SinkKind::Alloc);
  EXPECT_EQ(C.matchSink("_wcsdup"), C.matchSink("wcsdup"));
}

TEST(SinkCatalog, FallibleReleaseSummaryIsValidatedTransactionally) {
  const std::string Path =
      std::string(::testing::TempDir()) + "/neverd_fallible_release.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"custom_release","kind":"free",)"
       << R"("handle":0,"release_may_fail":true}]})";
  }
  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_FALSE(static_cast<bool>(C.mergeSinksFromFile(Path)));
  const SinkEntry *Release = C.matchSink("custom_release");
  ASSERT_NE(Release, nullptr);
  EXPECT_TRUE(Release->ReleaseMayFail);

  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"would_publish","kind":"free",)"
       << R"("handle":0},{"name":"bad","kind":"copy",)"
       << R"("dst":0,"release_may_fail":true}]})";
  }
  SinkCatalog Transactional = SinkCatalog::defaults();
  llvm::Error Error = Transactional.mergeSinksFromFile(Path);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("only a release"),
            std::string::npos);
  EXPECT_EQ(Transactional.matchSink("would_publish"), nullptr);
}

TEST(SinkCatalog, MangledOperatorNewAndDelete) {
  SinkCatalog C = SinkCatalog::defaults();
  // Itanium operator new / delete, in their platform underscore spellings.
  ASSERT_NE(C.matchSink("_Znwm"), nullptr);
  EXPECT_EQ(C.matchSink("_Znwm")->Kind, SinkKind::Alloc);
  ASSERT_NE(C.matchSink("_ZdlPv"), nullptr);
  EXPECT_EQ(C.matchSink("_ZdlPv")->Kind, SinkKind::Free);

  // Microsoft C++ names encode the target word size and overload signature.
  // They must resolve through the same allocation/lifetime summaries.
  ASSERT_NE(C.matchSink("??2@YAPEAX_K@Z"), nullptr);
  EXPECT_EQ(C.matchSink("??2@YAPEAX_K@Z")->Kind, SinkKind::Alloc);
  EXPECT_EQ(C.matchSink("__imp_??2@YAPEAX_K@Z"), C.matchSink("??2@YAPEAX_K@Z"));
  ASSERT_NE(C.matchSink("??2@YAPAXI@Z"), nullptr);
  EXPECT_EQ(C.matchSink("??2@YAPAXI@Z")->Kind, SinkKind::Alloc);
  ASSERT_NE(C.matchSink("??_U@YAPEAX_K@Z"), nullptr);
  EXPECT_EQ(C.matchSink("??_U@YAPEAX_K@Z")->Kind, SinkKind::Alloc);
  ASSERT_NE(C.matchSink("??3@YAXPEAX@Z"), nullptr);
  EXPECT_EQ(C.matchSink("??3@YAXPEAX@Z")->Kind, SinkKind::Free);
  ASSERT_NE(C.matchSink("??3@YAXPEAX_K@Z"), nullptr);
  EXPECT_EQ(C.matchSink("??3@YAXPEAX_K@Z")->Kind, SinkKind::Free);
  ASSERT_NE(C.matchSink("??_V@YAXPEAX@Z"), nullptr);
  EXPECT_EQ(C.matchSink("??_V@YAXPEAX@Z")->Kind, SinkKind::Free);
  EXPECT_EQ(C.matchSink("??2@YAPEAX_K@Z.trailing"), nullptr);
}

TEST(SinkCatalog, StandardOperatorVariantsAreExactAcrossABIs) {
  SinkCatalog C = SinkCatalog::defaults();
  for (const char *Name : {
           "_ZnwmRKSt9nothrow_t",
           "_ZnwjRKSt9nothrow_t",
           "_ZnwmSt11align_val_tRKSt9nothrow_t",
           "_ZnwjSt11align_val_tRKSt9nothrow_t",
           "_ZnamRKSt9nothrow_t",
           "_ZnajRKSt9nothrow_t",
           "_ZnamSt11align_val_tRKSt9nothrow_t",
           "_ZnajSt11align_val_tRKSt9nothrow_t",
           "??2@YAPEAX_KW4align_val_t@std@@AEBUnothrow_t@1@@Z",
           "??2@YAPAXIW4align_val_t@std@@ABUnothrow_t@1@@Z",
           "??_U@YAPEAX_KAEBUnothrow_t@std@@@Z",
           "??_U@YAPAXIABUnothrow_t@std@@@Z",
       }) {
    SCOPED_TRACE(Name);
    const SinkEntry *Entry = C.matchSink(Name);
    ASSERT_NE(Entry, nullptr);
    EXPECT_EQ(Entry->Kind, SinkKind::Alloc);
  }

  for (const char *Name : {
           "_ZdlPvj",
           "_ZdaPvj",
           "_ZdlPvjSt11align_val_t",
           "_ZdaPvjSt11align_val_t",
           "_ZdlPvRKSt9nothrow_t",
           "_ZdaPvSt11align_val_tRKSt9nothrow_t",
           "??3@YAXPEAX_KW4align_val_t@std@@@Z",
           "??3@YAXPAXIW4align_val_t@std@@@Z",
           "??_V@YAXPEAXAEBUnothrow_t@std@@@Z",
           "??_V@YAXPAXW4align_val_t@std@@ABUnothrow_t@1@@Z",
       }) {
    SCOPED_TRACE(Name);
    const SinkEntry *Entry = C.matchSink(Name);
    ASSERT_NE(Entry, nullptr);
    EXPECT_EQ(Entry->Kind, SinkKind::Free);
  }
}

TEST(SinkCatalog, RejectsQualifiedAndPlacementOperatorLookalikes) {
  SinkCatalog C = SinkCatalog::defaults();
  for (const char *Name : {
           "??2@YAPEAX_KPEAX@Z",
           "??_U@YAPAXIPAX@Z",
           "??3@YAXPEAX0@Z",
           "??_V@YAXPAX0@Z",
           "??2X@@SAPEAX_K@Z",
           "??_UX@@SAPAXI@Z",
           "??3X@@SAXPEAX@Z",
           "??_VX@@SAXPAX@Z",
           "_ZnwmPv",
           "_ZdlPvS_",
           "_ZN1X6memcpyEPvS0_m",
           "_ZN1X6mallocEm",
           "_ZN1X4freeEPv",
       }) {
    SCOPED_TRACE(Name);
    EXPECT_EQ(C.matchSink(Name), nullptr);
  }
}

TEST(SinkCatalog, ItaniumAlignedOperatorNewAndDelete) {
  SinkCatalog C = SinkCatalog::defaults();
  for (const char *Name : {"_ZnwmSt11align_val_t", "_ZnwjSt11align_val_t",
                           "_ZnamSt11align_val_t", "_ZnajSt11align_val_t"}) {
    SCOPED_TRACE(Name);
    const SinkEntry *Entry = C.matchSink(Name);
    ASSERT_NE(Entry, nullptr);
    EXPECT_EQ(Entry->Kind, SinkKind::Alloc);
  }
  for (const char *Name : {"_ZdlPvSt11align_val_t", "_ZdlPvmSt11align_val_t",
                           "_ZdaPvSt11align_val_t", "_ZdaPvmSt11align_val_t"}) {
    SCOPED_TRACE(Name);
    const SinkEntry *Entry = C.matchSink(Name);
    ASSERT_NE(Entry, nullptr);
    EXPECT_EQ(Entry->Kind, SinkKind::Free);
  }
}

TEST(SinkCatalog, ArmEabiMemoryHelpersPreserveArgumentLayouts) {
  SinkCatalog C = SinkCatalog::defaults();
  for (const char *Name :
       {"__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8",
        "__aeabi_memmove", "__aeabi_memmove4", "__aeabi_memmove8"}) {
    SCOPED_TRACE(Name);
    const SinkEntry *Entry = C.matchSink(Name);
    ASSERT_NE(Entry, nullptr);
    EXPECT_EQ(Entry->Kind, SinkKind::Copy);
    EXPECT_EQ(Entry->DstArg, 0);
    EXPECT_EQ(Entry->SrcArg, 1);
    EXPECT_EQ(Entry->LenArg, 2);
  }
  for (const char *Name :
       {"__aeabi_memset", "__aeabi_memset4", "__aeabi_memset8",
        "__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8"}) {
    SCOPED_TRACE(Name);
    const SinkEntry *Entry = C.matchSink(Name);
    ASSERT_NE(Entry, nullptr);
    EXPECT_EQ(Entry->Kind, SinkKind::Copy);
    EXPECT_EQ(Entry->DstArg, 0);
    EXPECT_EQ(Entry->SrcArg, -1);
    EXPECT_EQ(Entry->LenArg, 1);
  }
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
  EXPECT_TRUE(Read->returnCarriesInput());
  for (const char *Name :
       {"__read_chk", "__pread_chk", "__recv_chk", "__recvfrom_chk"}) {
    SCOPED_TRACE(Name);
    const SourceEntry *Source = C.matchSource(Name);
    ASSERT_NE(Source, nullptr);
    EXPECT_EQ(Source->OutArg, 1);
    EXPECT_TRUE(Source->returnCarriesInput());
  }
  for (const char *Name :
       {"fgets_unlocked", "fread_unlocked", "__fgets_chk", "__fread_chk",
        "__fgets_unlocked_chk", "__fread_unlocked_chk"}) {
    SCOPED_TRACE(Name);
    const SourceEntry *Source = C.matchSource(Name);
    ASSERT_NE(Source, nullptr);
    EXPECT_EQ(Source->OutArg, 0);
    EXPECT_TRUE(Source->returnCarriesInput());
  }
  ASSERT_NE(C.matchSource("scanf"), nullptr);
  EXPECT_FALSE(C.matchSource("scanf")->returnCarriesInput());
  ASSERT_NE(C.matchSource("ReadFile"), nullptr);
  EXPECT_EQ(C.matchSource("__imp__ReadFile@20"), C.matchSource("ReadFile"));
  EXPECT_EQ(C.matchSource("__imp_@ReadFile@20"), C.matchSource("ReadFile"));
  EXPECT_FALSE(C.matchSource("ReadFile")->returnCarriesInput());
  // PE/ucrt spells the same source as `_read`; the leading underscore folds.
  EXPECT_EQ(C.matchSource("_read"), Read);
}

TEST(SinkCatalog, SourceReturnSemanticsAreConfigurable) {
  std::string Path =
      std::string(::testing::TempDir()) + "/neverd_source_returns.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sources":[{"name":"implicit_return","out":-1},)"
       << R"({"name":"output_and_count","out":0,"return_tainted":true},)"
       << R"({"name":"scalar_status","out":-1,"return_tainted":false}]})";
  }
  SinkCatalog C = SinkCatalog::defaults();
  ASSERT_FALSE((bool)C.mergeSourcesFromFile(Path));
  ASSERT_NE(C.matchSource("implicit_return"), nullptr);
  EXPECT_TRUE(C.matchSource("implicit_return")->returnCarriesInput());
  ASSERT_NE(C.matchSource("output_and_count"), nullptr);
  EXPECT_TRUE(C.matchSource("output_and_count")->returnCarriesInput());
  ASSERT_NE(C.matchSource("scalar_status"), nullptr);
  EXPECT_FALSE(C.matchSource("scalar_status")->returnCarriesInput());
}

TEST(SinkCatalog, InvalidSourceReturnSemanticsAreTransactional) {
  std::string Path =
      std::string(::testing::TempDir()) + "/neverd_bad_source_return.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sources":[{"name":"would_publish","out":-1},)"
       << R"({"name":"bad","out":0,"return_tainted":"yes"}]})";
  }
  SinkCatalog C = SinkCatalog::defaults();
  llvm::Error Err = C.mergeSourcesFromFile(Path);
  ASSERT_TRUE(static_cast<bool>(Err));
  EXPECT_NE(llvm::toString(std::move(Err)).find("return_tainted"),
            std::string::npos);
  EXPECT_EQ(C.matchSource("would_publish"), nullptr);
}

TEST(SafetyVocab, JsonSpellingsComeFromTheEnumTable) {
  EXPECT_STREQ(toString(Track::Hunt), "hunt");
  EXPECT_STREQ(toString(Track::Audit), "audit");
  EXPECT_STREQ(toString(Verdict::Unsafe), "UNSAFE");
  EXPECT_STREQ(toString(SinkKind::Copy), "copy");
  EXPECT_STREQ(toString(VulnClass::HeapLeak), "heap_leak");
  EXPECT_STREQ(toString(VulnClass::UninitializedRead), "uninitialized_read");
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

TEST(SinkCatalog, RejectsUnknownSinkKind) {
  std::string Path =
      std::string(::testing::TempDir()) + "/neverd_bad_sink_kind.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"bad","kind":"not-a-kind"}]})";
  }
  SinkCatalog C = SinkCatalog::defaults();
  llvm::Error Err = C.mergeSinksFromFile(Path);
  ASSERT_TRUE(static_cast<bool>(Err));
  EXPECT_NE(llvm::toString(std::move(Err)).find("unknown sink kind"),
            std::string::npos);
  EXPECT_EQ(C.matchSink("bad"), nullptr);
}

TEST(SinkCatalog, RejectsOutOfRangeNumericFields) {
  std::string Path =
      std::string(::testing::TempDir()) + "/neverd_bad_sink_number.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sinks":[{"name":"bad","kind":"copy","dst":4294967296,)"
       << R"("severity":-1}]})";
  }
  SinkCatalog C = SinkCatalog::defaults();
  llvm::Error Err = C.mergeSinksFromFile(Path);
  ASSERT_TRUE(static_cast<bool>(Err));
  EXPECT_NE(llvm::toString(std::move(Err)).find("out of range"),
            std::string::npos);
  EXPECT_EQ(C.matchSink("bad"), nullptr);
}

TEST(SinkCatalog, SourceOverridesAreTransactional) {
  std::string Path =
      std::string(::testing::TempDir()) + "/neverd_bad_sources.json";
  {
    std::ofstream OS(Path);
    OS << R"({"sources":[{"name":"would_publish","out":0},)"
       << R"({"name":"bad","out":4294967296}]})";
  }
  SinkCatalog C = SinkCatalog::defaults();
  llvm::Error Err = C.mergeSourcesFromFile(Path);
  ASSERT_TRUE(static_cast<bool>(Err));
  EXPECT_NE(llvm::toString(std::move(Err)).find("out of range"),
            std::string::npos);
  EXPECT_EQ(C.matchSource("would_publish"), nullptr);
}
