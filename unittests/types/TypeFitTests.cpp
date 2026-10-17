//===- TypeFitTests.cpp - Local types that fit a pointer's accesses -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/TypeFit.h"
#include "neverd/types/TypeJSON.h"
#include "neverd/types/TypeParse.h"

#include <gtest/gtest.h>

using namespace neverd::types;

namespace {

/// A kernel APC as Windows 10 lays it out, a list entry, a record that
/// begins as an APC does, and the typedefs the headers declare.
constexpr const char *Kernel = R"(
  struct _LIST_ENTRY { struct _LIST_ENTRY *Flink; struct _LIST_ENTRY *Blink; };
  struct _KTHREAD;
  typedef struct _KAPC
  {
      UCHAR Type;                                    // 0x00
      UCHAR AllFlags;                                // 0x01
      UCHAR Size;                                    // 0x02
      UCHAR SpareByte1;                              // 0x03
      ULONG SpareLong0;                              // 0x04
      struct _KTHREAD *Thread;                       // 0x08
      struct _LIST_ENTRY ApcListEntry;               // 0x10
      union                                          // 0x20
      {
          void *Reserved[3];
          struct
          {
              void *KernelRoutine;                   // 0x20
              void *RundownRoutine;                  // 0x28
              void *NormalRoutine;                   // 0x30
          };
      };
      void *NormalContext;                           // 0x38
      void *SystemArgument1;                         // 0x40
      void *SystemArgument2;                         // 0x48
      CHAR ApcStateIndex;                            // 0x50
      CHAR ApcMode;                                  // 0x51
      UCHAR Inserted;                                // 0x52
  } KAPC, *PKAPC, *PRKAPC;
  struct _KAPC_STATE
  {
      struct _LIST_ENTRY ApcListHead[2];             // 0x00
      void *Process;                                 // 0x20
      UCHAR InProgressFlags;                         // 0x28
  };
  struct _HEADER_LIKE { UCHAR Type; UCHAR Flags; UCHAR Size; UCHAR Spare; };
  union _LARGE_INTEGER
  {
      struct { ULONG LowPart; LONG HighPart; };
      struct { ULONG LowPart; LONG HighPart; } u;
      LONGLONG QuadPart;
  };
  struct _FLAGS
  {
      ULONG Active : 1;
      ULONG Kind : 3;
      ULONG Spare : 28;
      UCHAR Data[16];
  };
)";

TypeLibrary kernel() {
  TypeLibrary Library;
  const ParseResult Result = parseCTypes(Kernel, Library);
  for (const SourceProblem &Problem : Result.Problems)
    ADD_FAILURE() << Problem.Line << ":" << Problem.Column << ": "
                  << Problem.Message;
  return Library;
}

const RecordDef &recordOf(const TypeLibrary &Library, llvm::StringRef Name) {
  static const RecordDef Empty;
  const LocalType *Type = Library.find(Name);
  EXPECT_TRUE(Type && Type->Record) << Name.str();
  return Type && Type->Record ? *Type->Record : Empty;
}

/// Where an access lands, as `fit path`.
std::string landing(const TypeLibrary &Library, llvm::StringRef Name,
                    int64_t Offset, uint64_t Bytes) {
  const MemberAt At = memberAt(Library, recordOf(Library, Name), Offset, Bytes);
  static const char *const Fits[] = {"exact", "start", "inside", "padding",
                                     "outside"};
  std::string Text = Fits[static_cast<int>(At.Fit)];
  if (!At.Path.empty())
    Text += " " + memberPathText(At.Path);
  return Text;
}

/// The accesses KeInitializeApc makes through its first argument.
std::vector<PointerAccess> initializeApc() {
  std::vector<PointerAccess> Accesses;
  for (auto [Offset, Bytes] : {std::pair<int64_t, uint64_t>{0x00, 1},
                               {0x02, 1},
                               {0x50, 1},
                               {0x28, 8},
                               {0x08, 8},
                               {0x30, 8},
                               {0x20, 8},
                               {0x51, 1},
                               {0x38, 8},
                               {0x52, 1},
                               {0x01, 1}})
    Accesses.push_back({Offset, Bytes, AccessUse::Write});
  return Accesses;
}

} // namespace

TEST(TypeFit, LandsAccessesOnMembers) {
  const TypeLibrary Library = kernel();
  EXPECT_EQ(landing(Library, "_KAPC", 0x00, 1), "exact Type");
  EXPECT_EQ(landing(Library, "_KAPC", 0x02, 1), "exact Size");
  EXPECT_EQ(landing(Library, "_KAPC", 0x08, 8), "exact Thread");
  // A list entry read in part is its member; read whole, it is itself.
  EXPECT_EQ(landing(Library, "_KAPC", 0x18, 8), "exact ApcListEntry.Blink");
  EXPECT_EQ(landing(Library, "_KAPC", 0x10, 16), "exact ApcListEntry");
  EXPECT_EQ(landing(Library, "_KAPC", 0x04, 2), "start SpareLong0");
  EXPECT_EQ(landing(Library, "_KAPC", 0x05, 1), "inside SpareLong0");
  EXPECT_EQ(landing(Library, "_KAPC", 0x53, 1), "padding");
  EXPECT_EQ(landing(Library, "_KAPC", 0x58, 1), "outside");
  EXPECT_EQ(landing(Library, "_KAPC", -1, 1), "outside");
  // An address lands on the outermost member that begins there.
  EXPECT_EQ(landing(Library, "_KAPC", 0x10, 0), "exact ApcListEntry");
  EXPECT_EQ(landing(Library, "_KAPC", 0x00, 0), "exact");
}

TEST(TypeFit, PrefersNamedMembersOfAUnion) {
  const TypeLibrary Library = kernel();
  // The routines over Reserved[0..2], which share their bytes.
  EXPECT_EQ(landing(Library, "_KAPC", 0x20, 8), "exact KernelRoutine");
  EXPECT_EQ(landing(Library, "_KAPC", 0x28, 8), "exact RundownRoutine");
  // The scalar over the records of its width.
  EXPECT_EQ(landing(Library, "_LARGE_INTEGER", 0, 8), "exact QuadPart");
  EXPECT_EQ(landing(Library, "_LARGE_INTEGER", 4, 4), "exact HighPart");
}

TEST(TypeFit, LandsOnBitfieldsAndElements) {
  const TypeLibrary Library = kernel();
  const MemberAt Unit = memberAt(Library, recordOf(Library, "_FLAGS"), 0, 4);
  EXPECT_EQ(Unit.Fit, AccessFit::Exact);
  EXPECT_TRUE(Unit.Bitfield);
  EXPECT_EQ(memberPathText(Unit.Path), "Active");
  EXPECT_EQ(landing(Library, "_FLAGS", 0, 1), "start Active");
  EXPECT_EQ(landing(Library, "_FLAGS", 7, 1), "exact Data[3]");
  EXPECT_EQ(landing(Library, "_KAPC_STATE", 0x18, 8),
            "exact ApcListHead[1].Blink");
}

TEST(TypeFit, MatchesNamesAsQuickOpenDoes) {
  const auto Score = [](llvm::StringRef Pattern, llvm::StringRef Text) {
    const std::optional<FuzzyMatch> Match = fuzzyMatch(Pattern, Text);
    return Match ? Match->Score : -1000000;
  };
  EXPECT_FALSE(fuzzyMatch("xyz", "_KAPC"));
  EXPECT_GT(Score("kapc", "_KAPC"), Score("kapc", "_KAPC_STATE"));
  EXPECT_GT(Score("kapc", "_KAPC_STATE"), Score("kapc", "PKAPC"));
  // The starts of words count.
  EXPECT_GT(Score("csl", "CurrentStackLocation"), Score("csl", "Classical"));
  const std::optional<FuzzyMatch> Match = fuzzyMatch("kapc", "_KAPC");
  ASSERT_TRUE(Match);
  EXPECT_EQ(Match->Runs, (std::vector<MatchRun>{{1, 4}}));
  const std::optional<FuzzyMatch> Words =
      fuzzyMatch("curstack", "Tail.Overlay.CurrentStackLocation");
  ASSERT_TRUE(Words);
  EXPECT_EQ(Words->Runs, (std::vector<MatchRun>{{13, 3}, {20, 5}}));
}

TEST(TypeFit, RanksRecordsByTheAccessesTheyHold) {
  const TypeLibrary Library = kernel();
  FitQuery Query;
  Query.Accesses = initializeApc();
  size_t Total = 0;
  const std::vector<TypeFit> Found = fitTypes(Library, Query, &Total);
  ASSERT_FALSE(Found.empty());
  EXPECT_EQ(Total, 6u);
  const TypeFit &Best = Found.front();
  EXPECT_EQ(Best.Type->Name, "_KAPC");
  EXPECT_EQ(Best.Exact, 11u);
  EXPECT_EQ(Best.Outside, 0u);
  EXPECT_EQ(Best.Aliases, (std::vector<std::string>{"KAPC"}));
  EXPECT_EQ(Best.PointerAliases, (std::vector<std::string>{"PKAPC", "PRKAPC"}));
  ASSERT_EQ(Best.Fits.size(), 11u);
  EXPECT_EQ(memberPathText(Best.Fits[3].Path), "RundownRoutine");
  // A record too small for them comes after those that hold them.
  EXPECT_GT(Found.back().Outside, 0u);
}

TEST(TypeFit, ASearchSpellsEachTypeAsItAlone) {
  // The types a search found share one reading of the library's names, and
  // read as each does alone.
  const TypeLibrary Library = kernel();
  FitQuery Query;
  Query.Accesses = initializeApc();
  const std::vector<TypeFit> Found = fitTypes(Library, Query);
  ASSERT_GT(Found.size(), 1u);
  for (BuiltinSpelling Spelling :
       {BuiltinSpelling::Portable, BuiltinSpelling::Windows}) {
    const llvm::json::Array All = typeFitsJSON(Library, Found, Spelling);
    ASSERT_EQ(All.size(), Found.size());
    for (size_t I = 0; I < Found.size(); ++I)
      EXPECT_EQ(All[I],
                llvm::json::Value(typeFitJSON(Library, Found[I], Spelling)))
          << Found[I].Type->Name;
  }
}

TEST(TypeFit, FindsWhereAPointerIntoARecordPoints) {
  const TypeLibrary Library = kernel();
  // A list entry's pointer, as an APC's list is walked: its own links, and
  // the APC's type and size before it.
  FitQuery Query;
  Query.Accesses = {{0x00, 8, AccessUse::Read},
                    {0x08, 8, AccessUse::Read},
                    {-0x10, 1, AccessUse::Read},
                    {-0x0e, 1, AccessUse::Read},
                    {0x40, 1, AccessUse::Read}};
  Query.Text = "kapc";
  const std::vector<TypeFit> Found = fitTypes(Library, Query);
  ASSERT_FALSE(Found.empty());
  const TypeFit &Best = Found.front();
  EXPECT_EQ(Best.Type->Name, "_KAPC");
  EXPECT_EQ(Best.Shift, 0x10u);
  EXPECT_EQ(memberPathText(Best.ShiftPath), "ApcListEntry");
  EXPECT_EQ(Best.Exact, 5u);
  EXPECT_EQ(memberPathText(Best.Fits[4].Path), "ApcStateIndex");
}

TEST(TypeFit, SearchesNamesAliasesAndMembers) {
  const TypeLibrary Library = kernel();
  FitQuery Query;
  Query.Text = "PKAPC";
  std::vector<TypeFit> Found = fitTypes(Library, Query);
  ASSERT_FALSE(Found.empty());
  EXPECT_EQ(Found.front().Type->Name, "_KAPC");
  EXPECT_EQ(Found.front().MatchedName, "PKAPC");

  Query.Text = ".rundown";
  Found = fitTypes(Library, Query);
  ASSERT_EQ(Found.size(), 1u);
  EXPECT_EQ(Found.front().Type->Name, "_KAPC");
  EXPECT_EQ(memberPathText(Found.front().MatchedMember), "RundownRoutine");

  Query.Text = "state->process";
  Found = fitTypes(Library, Query);
  ASSERT_EQ(Found.size(), 1u);
  EXPECT_EQ(Found.front().Type->Name, "_KAPC_STATE");

  // Every type under its own name; a typedef of a pointer to a record is
  // judged by that record.
  Query = FitQuery();
  Query.RecordsOnly = false;
  Query.Text = "PRKAPC";
  Query.Accesses = initializeApc();
  Found = fitTypes(Library, Query);
  ASSERT_FALSE(Found.empty());
  EXPECT_EQ(Found.front().Type->Name, "PRKAPC");
  ASSERT_NE(Found.front().Record, nullptr);
  EXPECT_EQ(Found.front().Record->Name, "_KAPC");
  EXPECT_EQ(Found.front().Exact, 11u);
}
