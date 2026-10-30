//===- BinaryDiffTests.cpp - Function correspondence and deltas -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The binary comparison over programs built by hand: identical, rebased,
/// reordered, stripped and renamed builds; constant, branch, call and
/// structure edits; ties that must stay ties; overrides; saved comparisons
/// and their refusal of other inputs; cancellation; and results that do not
/// depend on the number of threads.
///
//===----------------------------------------------------------------------===//

#include "neverd/diff/ChangeObservations.h"
#include "neverd/diff/Comparison.h"
#include "neverd/diff/ComparisonJSON.h"
#include "neverd/diff/FunctionDelta.h"
#include "neverd/diff/Program.h"
#include "neverd/diff/RegionsJSON.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace neverd::diff;

namespace {

uint64_t textHash(llvm::StringRef Text) {
  uint64_t H = 1469598103934665603ULL;
  for (unsigned char C : Text) {
    H ^= C;
    H *= 1099511628211ULL;
  }
  return H ? H : 1;
}

/// One instruction of a test function.  A call names its callee; an
/// operand's digits are its constants.
struct Op {
  std::string Mnemonic;
  std::string Operands;
  InstructionFlow Flow = InstructionFlow::None;
  std::string Callee;
};

struct Fn {
  std::string Name;
  NameSource Source = NameSource::Symbol;
  uint16_t Traits = 0;
  std::string Module;
  std::vector<std::vector<Op>> Blocks;
  std::vector<std::tuple<uint32_t, uint32_t, EdgeKind>> Edges;
  std::vector<std::string> Strings;
  std::vector<std::string> AddressTargets;
};

constexpr uint64_t OpSize = 6;

/// Lays \p Fns out from \p Base in \p Layout order (the listed order when
/// empty) and builds the program.
Program build(const std::vector<Fn> &Fns, uint64_t Base,
              std::vector<size_t> Layout = {}, std::string SHA = "",
              std::string Analysis = "test") {
  if (Layout.empty())
    for (size_t I = 0; I < Fns.size(); ++I)
      Layout.push_back(I);
  llvm::StringMap<uint64_t> Address;
  uint64_t Next = Base, NextImport = Base + 0x100000;
  std::vector<std::pair<uint64_t, size_t>> Placed;
  for (size_t I : Layout) {
    const Fn &F = Fns[I];
    if (F.Traits & TraitImport) {
      Address[F.Name] = NextImport;
      Placed.emplace_back(NextImport, I);
      NextImport += 8;
      continue;
    }
    Address[F.Name] = Next;
    Placed.emplace_back(Next, I);
    uint64_t Count = 0;
    for (const auto &B : F.Blocks)
      Count += B.size();
    Next += Count * OpSize + 16;
  }
  llvm::sort(Placed);
  ProgramIdentity Id;
  Id.Path = "test.bin";
  Id.SHA256 = SHA.empty() ? std::string(64, 'a') : SHA;
  Id.Format = "ELF";
  Id.Architecture = "x86_64";
  Id.Bits = 64;
  Id.Endian = "little";
  Id.ImageBase = Base;
  Id.Analysis = Analysis;
  ProgramBuilder B(Id);
  for (const auto &[At, I] : Placed) {
    const Fn &F = Fns[I];
    uint64_t Count = 0;
    for (const auto &Blk : F.Blocks)
      Count += Blk.size();
    B.beginFunction(At, Count * OpSize, F.Name, F.Source, F.Traits, F.Module);
    uint64_t PC = At;
    for (const auto &Blk : F.Blocks) {
      const uint32_t BI = B.addBlock(PC);
      for (const Op &O : Blk) {
        std::string Skeleton = O.Operands;
        for (char &C : Skeleton)
          if (C >= '0' && C <= '9')
            C = '#';
        std::vector<uint8_t> Bytes(OpSize);
        const uint64_t Opcode = textHash(O.Mnemonic);
        Bytes[0] = uint8_t(Opcode);
        Bytes[1] = uint8_t(Opcode >> 8);
        std::optional<uint64_t> Target;
        uint64_t Payload = textHash(O.Operands);
        uint64_t Shape = textHash(O.Mnemonic + " " + O.Operands);
        uint64_t Skel = textHash(O.Mnemonic + " " + Skeleton);
        if (!O.Callee.empty()) {
          Target = Address.lookup(O.Callee);
          Payload = *Target - (PC + OpSize);
          Shape = Skel = textHash(O.Mnemonic + " <function>");
        }
        for (unsigned K = 0; K < 4; ++K)
          Bytes[2 + K] = uint8_t(Payload >> (8 * K));
        std::vector<AddressReference> Addresses;
        if (PC == At)
          for (uint32_t N = 0; N < F.AddressTargets.size(); ++N)
            Addresses.push_back({Address.lookup(F.AddressTargets[N]), N});
        B.addInstruction(PC, Bytes, O.Mnemonic, Shape, Skel, O.Flow, Target, {},
                         {}, Addresses);
        PC += OpSize;
      }
      for (const std::string &S : F.Strings)
        if (BI == 0)
          B.addReference(ReferenceKind::String, textHash(S), 0);
    }
    for (const auto &[From, To, Kind] : F.Edges)
      B.addEdge(From, To, Kind);
  }
  return B.finish();
}

std::vector<Op> body(unsigned Seed, unsigned Count) {
  std::vector<Op> Ops;
  for (unsigned I = 0; I < Count; ++I)
    Ops.push_back(
        {I % 3 ? "mov" : "add", "r" + std::to_string((Seed + I) % 7) + ", " +
                                    std::to_string(Seed * 131 + I * 7)});
  return Ops;
}

Fn leaf(std::string Name, unsigned Seed, unsigned Count = 6) {
  Fn F;
  F.Name = std::move(Name);
  auto Ops = body(Seed, Count);
  Ops.push_back({"ret", ""});
  F.Blocks.push_back(std::move(Ops));
  return F;
}

/// if (x == Limit) A else B; join.
Fn diamond(std::string Name, unsigned Seed, unsigned Limit = 10,
           bool Inverted = false) {
  Fn F;
  F.Name = std::move(Name);
  auto Entry = body(Seed, 3);
  Entry.push_back({"cmp", "r1, " + std::to_string(Limit)});
  Entry.push_back({Inverted ? "jnz" : "jz", "", InstructionFlow::Branch});
  auto Then = body(Seed + 1, 4);
  Then.push_back({"jmp", "", InstructionFlow::Jump});
  auto Else = body(Seed + 2, 5);
  auto Join = body(Seed + 3, 2);
  Join.push_back({"ret", "", InstructionFlow::Return});
  if (!Inverted) {
    F.Blocks = {Entry, Then, Else, Join};
    F.Edges = {{0, 1, EdgeKind::Taken},
               {0, 2, EdgeKind::NotTaken},
               {1, 3, EdgeKind::Jump},
               {2, 3, EdgeKind::Fallthrough}};
  } else {
    // The same logic with the condition and the arms' layout swapped.
    auto Else2 = Else;
    Else2.push_back({"jmp", "", InstructionFlow::Jump});
    auto Then2 = Then;
    Then2.pop_back();
    F.Blocks = {Entry, Else2, Then2, Join};
    F.Edges = {{0, 2, EdgeKind::NotTaken},
               {0, 1, EdgeKind::Taken},
               {1, 3, EdgeKind::Jump},
               {2, 3, EdgeKind::Fallthrough}};
  }
  return F;
}

/// A loop calling \p Callees.
Fn caller(std::string Name, unsigned Seed, std::vector<std::string> Callees) {
  Fn F;
  F.Name = std::move(Name);
  auto Head = body(Seed, 2);
  auto Loop = body(Seed + 5, 2);
  for (const std::string &C : Callees)
    Loop.push_back({"call", "", InstructionFlow::Call, C});
  Loop.push_back({"cmp", "r2, 0"});
  Loop.push_back({"jnz", "", InstructionFlow::Branch});
  std::vector<Op> Exit = {{"ret", "", InstructionFlow::Return}};
  F.Blocks = {Head, Loop, Exit};
  F.Edges = {{0, 1, EdgeKind::Fallthrough},
             {1, 1, EdgeKind::Taken},
             {1, 2, EdgeKind::NotTaken}};
  return F;
}

Fn import(std::string Name, std::string Module = "libc.so.6") {
  Fn F;
  F.Name = std::move(Name);
  F.Source = NameSource::Import;
  F.Traits = TraitImport;
  F.Module = std::move(Module);
  return F;
}

/// A small program: imports, leaves, branches and callers.
std::vector<Fn> sample() {
  std::vector<Fn> Fns;
  Fns.push_back(import("malloc"));
  Fns.push_back(import("free"));
  Fns.push_back(import("memcpy"));
  for (unsigned I = 0; I < 6; ++I)
    Fns.push_back(leaf("leaf" + std::to_string(I), 10 + I * 3, 5 + I));
  for (unsigned I = 0; I < 4; ++I)
    Fns.push_back(diamond("check" + std::to_string(I), 50 + I * 4, 10 + I));
  Fn Alloc = caller("alloc_copy", 90, {"malloc", "memcpy", "leaf1"});
  Alloc.Strings = {"out of memory"};
  Fns.push_back(Alloc);
  Fns.push_back(caller("release", 95, {"free", "leaf2"}));
  Fns.push_back(caller(
      "driver", 100,
      {"alloc_copy", "check0", "check1", "release", "check2", "check3"}));
  return Fns;
}

const Function &named(const Program &P, llvm::StringRef Name) {
  for (const Function &F : P.Functions)
    if (F.Name == Name)
      return F;
  ADD_FAILURE() << "no function " << Name.str();
  return P.Functions.front();
}

uint32_t indexOf(const Program &P, llvm::StringRef Name) {
  return uint32_t(&named(P, Name) - P.Functions.data());
}

/// The secondary function's name that \p Name pairs with, or "".
std::string counterpart(const ComparisonResult &R, const Program &P,
                        const Program &S, llvm::StringRef Name) {
  const uint32_t Other = R.secondaryOf(indexOf(P, Name));
  return Other == NoIndex ? std::string() : S.Functions[Other].Name;
}

const FunctionMatch &matchOf(const ComparisonResult &R, const Program &P,
                             llvm::StringRef Name) {
  const FunctionMatch *M = R.matchOfPrimary(indexOf(P, Name));
  EXPECT_NE(M, nullptr) << Name.str();
  static FunctionMatch None;
  return M ? *M : None;
}

std::string saved(const ComparisonResult &R, const Program &P,
                  const Program &S) {
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  writeComparison(OS, R, P, S);
  return Out;
}

/// Saved text without timings, which differ between runs.
std::string savedWithoutTimings(ComparisonResult R, const Program &P,
                                const Program &S) {
  R.Timings.clear();
  return saved(R, P, S);
}

std::vector<Fn> stripped(std::vector<Fn> Fns) {
  llvm::StringMap<bool> Code;
  for (Fn &F : Fns)
    if (!(F.Traits & TraitImport)) {
      Code[F.Name] = true;
      F.Name = "sub_" + F.Name;
      F.Source = NameSource::Generated;
    }
  for (Fn &F : Fns)
    for (auto &Block : F.Blocks)
      for (Op &O : Block)
        if (!O.Callee.empty() && Code.count(O.Callee))
          O.Callee = "sub_" + O.Callee;
  return Fns;
}

} // namespace

TEST(BinaryDiff, ProgramsValidateTheirIndices) {
  Program P = build(sample(), 0x400000);
  EXPECT_FALSE(P.validate());
  Program Broken = P;
  Broken.Functions[5].FirstBlock = uint32_t(Broken.Blocks.size());
  EXPECT_TRUE(Broken.validate());
  Broken = P;
  std::swap(Broken.Functions[3], Broken.Functions[4]);
  EXPECT_TRUE(Broken.validate());
  Broken = P;
  Broken.Instructions[0].Callee = uint32_t(Broken.Functions.size());
  EXPECT_TRUE(Broken.validate());
}

TEST(BinaryDiff, IdenticalInputsHaveNoCodeChanges) {
  const Program P = build(sample(), 0x400000);
  const Program S = build(sample(), 0x400000);
  const ComparisonResult R = compare(P, S, {});
  EXPECT_EQ(R.State, ComparisonState::Complete);
  EXPECT_EQ(R.Matches.size(), P.Functions.size());
  for (const FunctionMatch &M : R.Matches) {
    EXPECT_EQ(P.Functions[M.Primary].Name, S.Functions[M.Secondary].Name);
    EXPECT_TRUE(M.Measured);
    EXPECT_EQ(M.Changes, 0u) << P.Functions[M.Primary].Name;
    EXPECT_EQ(M.Similarity, M.IdentityOnly ? 0.0 : 1.0);
    EXPECT_EQ(M.identical(), P.Functions[M.Primary].hasBody());
  }
  EXPECT_EQ(R.Similarity, 1.0);
  EXPECT_EQ(R.PrimaryCoverage.Unmatched, 0u);
}

TEST(BinaryDiff, IndistinguishableDuplicatesStayExplicit) {
  // Three byte-identical stubs no code calls: nothing can tell them apart.
  std::vector<Fn> Fns = sample();
  for (unsigned I = 0; I < 3; ++I) {
    Fn Stub = leaf("stub" + std::to_string(I), 7, 1);
    Stub.Source = NameSource::Generated;
    Stub.Name = "sub_stub" + std::to_string(I);
    Fns.push_back(Stub);
  }
  const Program P = build(Fns, 0x400000);
  const Program S = build(Fns, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  for (unsigned I = 0; I < 3; ++I)
    EXPECT_EQ(R.matchOfPrimary(indexOf(P, "sub_stub" + std::to_string(I))),
              nullptr);
  ASSERT_EQ(R.Ambiguities.size(), 1u);
  EXPECT_EQ(R.Ambiguities[0].Primary.size(), 3u);
  EXPECT_EQ(R.Ambiguities[0].Secondary.size(), 3u);
  EXPECT_EQ(R.PrimaryCoverage.Ambiguous, 3u);
  // Every function that can be told apart still pairs without changes.
  for (const FunctionMatch &M : R.Matches)
    EXPECT_EQ(M.Changes, 0u);
}

TEST(BinaryDiff, ZeroAmbiguityMarginStillPreservesExactTies) {
  std::vector<Fn> Fns;
  for (unsigned I = 0; I < 3; ++I) {
    Fns.push_back(leaf("duplicate" + std::to_string(I), 7, 1));
    Fns.back().Source = NameSource::Generated;
  }
  const Program P = build(Fns, 0x400000);
  const Program S = build(Fns, 0x500000, {2, 0, 1});
  ComparisonSettings Settings;
  Settings.AmbiguityMargin = 0;
  for (const auto &[A, B] : {std::pair(&P, &S), std::pair(&S, &P)}) {
    const auto R = compare(*A, *B, Settings);
    EXPECT_TRUE(R.Matches.empty());
    EXPECT_EQ(R.PrimaryCoverage.Ambiguous, 3u);
    EXPECT_EQ(R.SecondaryCoverage.Ambiguous, 3u);
  }
}

TEST(BinaryDiff, RelativeCallBytesCannotIdentifyDuplicateWrappers) {
  Fn Target = leaf("target", 91);
  Fn A = caller("wrapper_a", 7, {"target"});
  Fn B = A;
  B.Name = "wrapper_b";
  A.Source = B.Source = NameSource::Generated;
  const Program P = build({Target, A, B}, 0x400000);
  // Reordering swaps which indistinguishable wrapper has the same encoded
  // relative displacement. That is layout evidence, not source identity.
  const Program S = build({Target, A, B}, 0x500000, {0, 2, 1});
  for (const auto &[Left, Right] : {std::pair(&P, &S), std::pair(&S, &P)}) {
    const auto R = compare(*Left, *Right, {});
    SCOPED_TRACE(saved(R, *Left, *Right));
    EXPECT_EQ(counterpart(R, *Left, *Right, "target"), "target");
    EXPECT_EQ(R.matchOfPrimary(indexOf(*Left, "wrapper_a")), nullptr);
    EXPECT_EQ(R.matchOfPrimary(indexOf(*Left, "wrapper_b")), nullptr);
    EXPECT_EQ(R.PrimaryCoverage.Ambiguous, 2U);
    EXPECT_EQ(R.SecondaryCoverage.Ambiguous, 2U);
  }
}

TEST(BinaryDiff, OverlappingSimilarityTiesKeepOnlyMeasuredAlternatives) {
  const auto F = [](const char *Name, const char *First, const char *Second) {
    Fn Out;
    Out.Name = Name;
    Out.Blocks = {{{First, "r0, r1"}, {Second, "r0, r1"}, {"ret", ""}}};
    return Out;
  };
  const std::vector<Fn> PFns = {F("a", "add", "sub"), F("b", "xor", "and"),
                                F("c", "or", "imul")};
  const std::vector<Fn> SFns = {F("x", "add", "xor"), F("y", "sub", "or"),
                                F("z", "and", "imul")};
  const std::set<std::pair<std::string, std::string>> Expected = {
      {"a", "x"}, {"a", "y"}, {"b", "x"}, {"b", "z"}, {"c", "y"}, {"c", "z"}};
  ComparisonSettings Settings;
  Settings.UseNames = false;
  Settings.MinimumSimilarity = 0.7;
  Settings.AmbiguityMargin = 0;
  for (const bool Reverse : {false, true})
    for (const auto &Layout :
         {std::vector<size_t>{0, 1, 2}, std::vector<size_t>{2, 0, 1}}) {
      const Program P = build(Reverse ? SFns : PFns, 0x400000, Layout);
      const Program S = build(Reverse ? PFns : SFns, 0x800000);
      const auto Key = [&](uint32_t A, uint32_t B) {
        const auto Pair = std::pair(P.Functions[A].Name, S.Functions[B].Name);
        return Reverse ? std::pair(Pair.second, Pair.first) : Pair;
      };
      // The independently specified six-cycle has no diagonal cross-pairs.
      for (uint32_t A = 0; A < P.Functions.size(); ++A)
        for (uint32_t B = 0; B < S.Functions.size(); ++B)
          EXPECT_EQ(compareFunctions(P, A, S, B, {}).Similarity >=
                        Settings.MinimumSimilarity,
                    Expected.contains(Key(A, B)));
      const auto R = compare(P, S, Settings);
      EXPECT_TRUE(R.Matches.empty());
      EXPECT_EQ(R.PrimaryCoverage.Ambiguous, 3u);
      EXPECT_EQ(R.SecondaryCoverage.Ambiguous, 3u);
      std::set<std::pair<std::string, std::string>> Actual;
      for (const auto &Group : R.Ambiguities)
        for (uint32_t A : Group.Primary)
          for (uint32_t B : Group.Secondary)
            Actual.insert(Key(A, B));
      EXPECT_EQ(Actual, Expected);
      std::string Error;
      const auto Read = readComparison(saved(R, P, S), Error);
      ASSERT_TRUE(Read) << Error;
      ComparisonResult Reopened;
      ASSERT_FALSE(bindComparison(*Read, P, S, Reopened));
      EXPECT_EQ(savedWithoutTimings(Reopened, P, S),
                savedWithoutTimings(R, P, S));
    }
}

TEST(BinaryDiff, ExactGroupsDoNotHideOtherMeasuredCandidates) {
  std::vector<Fn> PFns = {leaf("a", 7, 1), leaf("b", 7, 1), leaf("c", 8, 1)};
  std::vector<Fn> SFns = {leaf("x", 7, 1), leaf("y", 7, 1), leaf("z", 7, 1)};
  ComparisonSettings Settings;
  Settings.UseNames = false;
  Settings.MinimumSimilarity = 0.5;
  for (const bool Reverse : {false, true}) {
    const Program P = build(Reverse ? SFns : PFns, 0x400000);
    const Program S = build(Reverse ? PFns : SFns, 0x800000);
    const auto R = compare(P, S, Settings);
    EXPECT_TRUE(R.Matches.empty());
    EXPECT_EQ(R.PrimaryCoverage.Ambiguous, 3u);
    EXPECT_EQ(R.SecondaryCoverage.Ambiguous, 3u);
    std::set<std::pair<uint32_t, uint32_t>> Candidates;
    for (const auto &Group : R.Ambiguities)
      for (uint32_t A : Group.Primary)
        for (uint32_t B : Group.Secondary)
          Candidates.emplace(A, B);
    EXPECT_EQ(Candidates.size(), 9u);
  }
}

TEST(BinaryDiff, ConflictingRepeatedCallPositionsStayUnresolved) {
  std::vector<Fn> PFns = {leaf("a", 7, 1), leaf("b", 7, 1),
                          caller("dispatch", 120, {"a", "b", "a"})};
  std::vector<Fn> SFns = {leaf("x", 7, 1), leaf("y", 7, 1),
                          caller("dispatch", 120, {"x", "x", "y"})};
  for (auto *Fns : {&PFns, &SFns})
    for (unsigned I = 0; I < 2; ++I)
      (*Fns)[I].Source = NameSource::Generated;
  const Program P = build(PFns, 0x400000);
  const Program S = build(SFns, 0x500000);
  for (const auto &[A, B] : {std::pair(&P, &S), std::pair(&S, &P)}) {
    const auto R = compare(*A, *B, {});
    ASSERT_EQ(R.Matches.size(), 1u);
    EXPECT_EQ(A->Functions[R.Matches[0].Primary].Name, "dispatch");
    EXPECT_EQ(R.PrimaryCoverage.Ambiguous, 2u);
    EXPECT_EQ(R.SecondaryCoverage.Ambiguous, 2u);
  }
}

TEST(BinaryDiff, LongCallSequencesRetainContextualCorrespondence) {
  for (unsigned Repeats : {1200u, 70000u}) {
    std::vector<std::string> Calls = {"a"};
    Calls.insert(Calls.end(), Repeats, "anchor");
    Calls.push_back("b");
    std::vector<Fn> PFns = {leaf("a", 7, 1), leaf("b", 7, 1),
                            leaf("anchor", 19), caller("dispatch", 120, Calls)};
    PFns[0].Source = PFns[1].Source = NameSource::Generated;
    auto SFns = PFns;
    SFns[0].Name = "x";
    SFns[1].Name = "y";
    for (auto &Block : SFns[3].Blocks)
      for (auto &O : Block) {
        if (O.Callee == "a")
          O.Callee = "x";
        else if (O.Callee == "b")
          O.Callee = "y";
      }
    const Program P = build(PFns, 0x400000);
    const Program S = build(SFns, 0x800000, {1, 3, 0, 2});
    const auto R = compare(P, S, {});
    EXPECT_EQ(counterpart(R, P, S, "a"), "x") << Repeats;
    EXPECT_EQ(counterpart(R, P, S, "b"), "y") << Repeats;
    EXPECT_TRUE(R.Ambiguities.empty());
  }
}

TEST(BinaryDiff, RepeatedAnchorChoicesDoNotInventCalleePairs) {
  std::vector<Fn> PFns = {
      leaf("a", 7, 1), leaf("b", 7, 1), leaf("anchor", 19),
      caller("dispatch", 120, {"a", "anchor", "b", "anchor"})};
  std::vector<Fn> SFns = {leaf("x", 7, 1), leaf("y", 7, 1), leaf("anchor", 19),
                          caller("dispatch", 120, {"x", "anchor", "y"})};
  for (auto *Fns : {&PFns, &SFns})
    for (unsigned I = 0; I < 2; ++I)
      (*Fns)[I].Source = NameSource::Generated;
  const Program P = build(PFns, 0x400000);
  const Program S = build(SFns, 0x500000);
  for (const auto &[A, B] : {std::pair(&P, &S), std::pair(&S, &P)}) {
    const auto R = compare(*A, *B, {});
    EXPECT_EQ(R.Matches.size(), 2u);
    EXPECT_EQ(R.PrimaryCoverage.Ambiguous, 2u);
    EXPECT_EQ(R.SecondaryCoverage.Ambiguous, 2u);
  }
}

TEST(BinaryDiff, UnresolvedCallsKeepTheirPositionAndUncertainty) {
  for (InstructionFlow Flow :
       {InstructionFlow::Call, InstructionFlow::IndirectCall,
        InstructionFlow::Jump, InstructionFlow::Branch}) {
    std::vector<Fn> PFns = {leaf("a", 7, 1), leaf("b", 7, 1),
                            caller("dispatch", 120, {"a", "b"})};
    std::vector<Fn> SFns = {leaf("x", 7, 1), leaf("y", 7, 1),
                            caller("dispatch", 120, {"x", "y"})};
    PFns[2].Blocks[1].insert(
        PFns[2].Blocks[1].begin() + 3,
        {"transfer", "rax", Flow,
         Flow == InstructionFlow::Jump || Flow == InstructionFlow::Branch
             ? "omitted_target"
             : ""});
    for (auto *Fns : {&PFns, &SFns})
      for (unsigned I = 0; I < 2; ++I)
        (*Fns)[I].Source = NameSource::Generated;
    const Program P = build(PFns, 0x400000);
    const Program S = build(SFns, 0x500000);
    for (const auto &[A, B] : {std::pair(&P, &S), std::pair(&S, &P)}) {
      const auto R = compare(*A, *B, {});
      EXPECT_EQ(R.Matches.size(), 1u);
      EXPECT_EQ(R.PrimaryCoverage.Ambiguous, 2u);
      EXPECT_EQ(R.SecondaryCoverage.Ambiguous, 2u);
    }
  }
}

TEST(BinaryDiff, ConsistentCallPairsSurviveOtherPositionalConflicts) {
  for (bool Conflict : {false, true}) {
    std::vector<Fn> PFns = {leaf("a", 7, 1), leaf("b", 7, 1), leaf("c", 7, 1),
                            caller("dispatch", 120, {"a", "b", "a", "c", "c"})};
    std::vector<Fn> SFns = {
        leaf("x", 7, 1), leaf("y", 7, 1), leaf("z", 7, 1),
        caller("dispatch", 120,
               Conflict ? std::vector<std::string>{"x", "x", "y", "z", "z"}
                        : std::vector<std::string>{"x", "y", "x", "z", "z"})};
    for (auto *Fns : {&PFns, &SFns})
      for (unsigned I = 0; I < 3; ++I)
        (*Fns)[I].Source = NameSource::Generated;
    const Program P = build(PFns, 0x400000);
    const Program S = build(SFns, 0x500000, {2, 1, 0, 3});
    const auto R = compare(P, S, {});
    EXPECT_EQ(counterpart(R, P, S, "c"), "z");
    EXPECT_EQ(R.Matches.size(), Conflict ? 2u : 4u);
    EXPECT_EQ(R.PrimaryCoverage.Ambiguous, Conflict ? 2u : 0u);
    EXPECT_EQ(R.SecondaryCoverage.Ambiguous, Conflict ? 2u : 0u);
  }
}

TEST(BinaryDiff, ContextTellsApartDuplicatesItCalls) {
  // Two identical helpers, each called from one place of a unique caller.
  std::vector<Fn> Fns = sample();
  Fns.push_back(leaf("helper_a", 3, 2));
  Fns.push_back(leaf("helper_b", 3, 2));
  Fns.push_back(caller("dispatch", 120, {"helper_a", "leaf3", "helper_b"}));
  for (Fn &F : Fns)
    if (F.Name.rfind("helper", 0) == 0)
      F.Source = NameSource::Generated;
  const Program P = build(Fns, 0x400000);
  const Program S = build(Fns, 0x500000, {});
  const ComparisonResult R = compare(P, S, {});
  EXPECT_EQ(counterpart(R, P, S, "helper_a"), "helper_a");
  EXPECT_EQ(counterpart(R, P, S, "helper_b"), "helper_b");
  EXPECT_TRUE(R.Ambiguities.empty());
}

TEST(BinaryDiff, RebasedAndReorderedFunctionsOnlyMove) {
  const std::vector<Fn> Fns = sample();
  std::vector<size_t> Reversed;
  for (size_t I = Fns.size(); I-- > 0;)
    Reversed.push_back(I);
  const Program P = build(Fns, 0x400000);
  const Program S = build(stripped(Fns), 0x7ff000000, Reversed);
  const ComparisonResult R = compare(P, S, {});
  for (const Function &F : P.Functions) {
    const std::string Other = counterpart(R, P, S, F.Name);
    const bool Import = F.has(TraitImport);
    EXPECT_EQ(Other, Import ? F.Name : "sub_" + F.Name);
    const FunctionMatch &M = matchOf(R, P, F.Name);
    EXPECT_EQ(M.identical(), !Import)
        << F.Name << " " << changeLetters(M.Changes);
    EXPECT_EQ(M.IdentityOnly, Import);
    EXPECT_EQ(M.Similarity, Import ? 0.0 : 1.0) << F.Name;
  }
}

TEST(BinaryDiff, StrippedBuildsPairWithoutNames) {
  const std::vector<Fn> Fns = sample();
  const Program P = build(stripped(Fns), 0x400000);
  const Program S = build(stripped(Fns), 0x400000);
  ComparisonSettings NoNames;
  NoNames.UseNames = false;
  for (const ComparisonSettings &Settings : {ComparisonSettings(), NoNames}) {
    const ComparisonResult R = compare(P, S, Settings);
    for (const Function &F : P.Functions)
      EXPECT_EQ(counterpart(R, P, S, F.Name), F.Name);
  }
}

TEST(BinaryDiff, DuplicateNamesAreNotCollapsed) {
  std::vector<Fn> Fns = sample();
  Fn A = leaf("dup", 300, 7), B = diamond("dup", 400);
  B.Name = "dup";
  Fns.push_back(A);
  Fns.push_back(B);
  const Program P = build(Fns, 0x400000);
  const Program S = build(Fns, 0x480000);
  const ComparisonResult R = compare(P, S, {});
  unsigned Dups = 0;
  for (const FunctionMatch &M : R.Matches)
    if (P.Functions[M.Primary].Name == "dup") {
      ++Dups;
      EXPECT_EQ(P.Functions[M.Primary].BlockCount,
                S.Functions[M.Secondary].BlockCount);
      EXPECT_TRUE(M.identical());
    }
  EXPECT_EQ(Dups, 2u);
}

TEST(BinaryDiff, LongInstructionAlignmentPreservesSparseInsertions) {
  Fn Before = leaf("long_body", 41, 6000);
  auto After = Before;
  auto &Instructions = After.Blocks.front();
  Instructions.insert(Instructions.begin() + 1000, 40, {"mov", "r3, 999999"});
  Instructions.insert(Instructions.begin() + 4040, 32, {"add", "r2, 888888"});
  const Program P = build({Before}, 0x400000);
  const Program S = build({After}, 0x500000);
  for (const bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    const auto &A = Reverse ? S : P;
    const auto &B = Reverse ? P : S;
    const auto D = compareFunctions(A, 0, B, 0, {});
    ASSERT_TRUE(D.ComputationComplete);
    EXPECT_EQ(D.EqualInstructions, 6001u);
    unsigned Inserted = 0, Deleted = 0, Other = 0;
    uint32_t AI = 0, BI = 0;
    for (const auto &Row : D.Rows) {
      if (Row.Primary != NoIndex)
        EXPECT_EQ(Row.Primary, AI++);
      if (Row.Secondary != NoIndex)
        EXPECT_EQ(Row.Secondary, BI++);
      Inserted += Row.Kind == RowKind::Inserted;
      Deleted += Row.Kind == RowKind::Deleted;
      Other += Row.Kind != RowKind::Identical && Row.Kind != RowKind::Moved &&
               Row.Kind != RowKind::Inserted && Row.Kind != RowKind::Deleted;
    }
    EXPECT_EQ(AI, A.Instructions.size());
    EXPECT_EQ(BI, B.Instructions.size());
    EXPECT_EQ(Inserted, Reverse ? 0u : 72u);
    EXPECT_EQ(Deleted, Reverse ? 72u : 0u);
    EXPECT_EQ(Other, 0u);
  }
}

TEST(BinaryDiff, LargeInstructionAlignmentMatchesIndependentEditCost) {
  std::mt19937 Random(0xD1FF6001);
  for (const auto &[N, M] :
       {std::pair{2100u, 2200u}, {4700u, 4600u}, {97u, 44000u}}) {
    SCOPED_TRACE(N);
    SCOPED_TRACE(M);
    const auto Make = [&](unsigned Count) {
      Fn F;
      F.Name = "weighted";
      F.Blocks.resize(1);
      for (unsigned I = 0; I < Count; ++I) {
        const char *Mnemonics[] = {"mov", "add", "sub", "xor", "or"};
        F.Blocks[0].push_back(
            {Mnemonics[Random() % 5], std::string(Random() % 2 ? "r" : "v") +
                                          std::to_string(Random() % 3) + ", " +
                                          std::to_string(Random() % 17)});
      }
      return F;
    };
    const Program P = build({Make(N)}, 0x400000);
    const Program S = build({Make(M)}, 0x500000);
    // Independent forward edit-cost oracle: gaps cost two, and replacing
    // a row costs zero/one/two/four for shape/skeleton/mnemonic/no agreement.
    // Production maximizes weights, decomposing large ranges at a middle row.
    std::vector<uint32_t> Previous(M + 1), Current(M + 1);
    for (size_t J = 0; J <= M; ++J)
      Previous[J] = 2 * J;
    for (size_t I = 1; I <= N; ++I) {
      Current[0] = 2 * I;
      const auto &X = P.Instructions[I - 1];
      for (size_t J = 1; J <= M; ++J) {
        const auto &Y = S.Instructions[J - 1];
        const unsigned Cost =
            X.Shape == Y.Shape                                   ? 0
            : X.Skeleton == Y.Skeleton                           ? 1
            : P.Mnemonics[X.Mnemonic] == S.Mnemonics[Y.Mnemonic] ? 2
                                                                 : 4;
        Current[J] = std::min(
            {Previous[J] + 2, Current[J - 1] + 2, Previous[J - 1] + Cost});
      }
      std::swap(Previous, Current);
    }
    for (const bool Reverse : {false, true}) {
      SCOPED_TRACE(Reverse);
      const auto &A = Reverse ? S : P;
      const auto &B = Reverse ? P : S;
      const auto D = compareFunctions(A, 0, B, 0, {});
      ASSERT_TRUE(D.ComputationComplete);
      EXPECT_EQ(D.State, ComparisonState::Complete);
      uint32_t AI = 0, BI = 0, Cost = 0;
      for (const auto &Row : D.Rows) {
        if (Row.Primary != NoIndex)
          EXPECT_EQ(Row.Primary, AI++);
        if (Row.Secondary != NoIndex)
          EXPECT_EQ(Row.Secondary, BI++);
        switch (Row.Kind) {
        case RowKind::Identical:
        case RowKind::Moved:
          ASSERT_NE(Row.Primary, NoIndex);
          ASSERT_NE(Row.Secondary, NoIndex);
          EXPECT_EQ(A.Instructions[Row.Primary].Shape,
                    B.Instructions[Row.Secondary].Shape);
          break;
        case RowKind::Constant:
          ++Cost;
          break;
        case RowKind::Operand:
        case RowKind::Inserted:
        case RowKind::Deleted:
          Cost += 2;
          break;
        case RowKind::Replaced:
          Cost += 4;
          break;
        case RowKind::Uncertain:
          FAIL() << "Scalar-only alignment cannot have a reference gap";
          break;
        }
      }
      EXPECT_EQ(AI, A.Instructions.size());
      EXPECT_EQ(BI, B.Instructions.size());
      EXPECT_EQ(Cost, Previous.back());
    }
  }
}

TEST(BinaryDiff, LongConstantEditsRetainEveryUnchangedInstruction) {
  Fn Before;
  Before.Name = "constants";
  Before.Blocks.resize(1);
  for (unsigned I = 0; I < 100000; ++I)
    Before.Blocks[0].push_back({"mov", "r0, " + std::to_string(100000 + I)});
  auto After = Before;
  for (unsigned I = 0; I < 100000; I += 10000)
    After.Blocks[0][I].Operands = "r0, " + std::to_string(600000 + I);
  Program P = build({Before}, 0x400000);
  Program S = build({After}, 0x800000);
  // Shape hashes cover the full nonzero uint64_t domain. These valid values
  // must not be mistaken for a hash container's empty/tombstone sentinels.
  for (auto *Input : {&P, &S}) {
    Input->Instructions[1].Shape = UINT64_MAX;
    Input->Instructions[2].Shape = UINT64_MAX - 1;
    ASSERT_FALSE(Input->validate());
  }
  const auto D = compareFunctions(P, 0, S, 0, {});
  ASSERT_TRUE(D.ComputationComplete);
  ASSERT_EQ(D.Rows.size(), 100000u);
  EXPECT_EQ(D.EqualInstructions, 99990u);
  for (size_t I = 0; I < D.Rows.size(); ++I) {
    EXPECT_EQ(D.Rows[I].Primary, I);
    EXPECT_EQ(D.Rows[I].Secondary, I);
    EXPECT_EQ(D.Rows[I].Kind,
              I % 10000 ? RowKind::Identical : RowKind::Constant);
  }
}

TEST(BinaryDiff, LongAlignmentStopsWithoutPublishingPartialRows) {
  const Program P = build({leaf("long", 11, 40000)}, 0x400000);
  const Program S = build({leaf("long", 91, 40000)}, 0x800000);
  for (const bool Timed : {false, true}) {
    SCOPED_TRACE(Timed);
    std::atomic<bool> Cancel{false};
    ComparisonControl Control;
    if (Timed)
      Control.Deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
    else
      Control.Cancel = &Cancel;
    std::thread Stop([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      Cancel.store(true);
    });
    const auto D = compareFunctions(P, 0, S, 0, {}, Control);
    Stop.join();
    EXPECT_EQ(D.State,
              Timed ? ComparisonState::TimedOut : ComparisonState::Cancelled);
    EXPECT_FALSE(D.ComputationComplete);
    EXPECT_FALSE(D.AnalysisComplete);
    EXPECT_TRUE(D.Rows.empty());
    EXPECT_TRUE(D.Blocks.empty());
    EXPECT_EQ(D.EqualInstructions, 0u);
  }
}

TEST(BinaryDiff, LongAlignmentMeasurementsRoundTripAndRefuseStaleScores) {
  Fn Before = leaf("long_body", 41, 6000);
  auto After = Before;
  After.Blocks[0].insert(After.Blocks[0].begin() + 1000, 40,
                         {"mov", "r3, 999999"});
  const Program P = build({Before}, 0x400000);
  const Program S = build({After}, 0x500000);
  const auto R = compare(P, S, {});
  ASSERT_EQ(R.Matches.size(), 1u);
  EXPECT_EQ(R.Matches[0].EqualInstructions, 6001u);
  std::string Error;
  auto Read = readComparison(saved(R, P, S), Error);
  ASSERT_TRUE(bool(Read)) << Error;
  EXPECT_EQ(Read->MatcherVersion, MatcherVersion);
  ComparisonResult Restored;
  ASSERT_FALSE(bindComparison(*Read, P, S, Restored));
  EXPECT_EQ(savedWithoutTimings(R, P, S), savedWithoutTimings(Restored, P, S));
  // An older version with still-valid measurements may rebind with a warning.
  Read->MatcherVersion = 2;
  ASSERT_FALSE(bindComparison(*Read, P, S, Restored));
  EXPECT_FALSE(Restored.Warnings.empty());
  const auto Previous = savedWithoutTimings(Restored, P, S);
  // Historical heuristics or tampering cannot authorize stale measurements.
  --Read->Matches[0].Match.EqualInstructions;
  BindFailure Kind;
  const auto Failure = bindComparison(*Read, P, S, Restored, &Kind);
  ASSERT_TRUE(Failure);
  EXPECT_EQ(Kind, BindFailure::InvalidEvidence);
  EXPECT_EQ(savedWithoutTimings(Restored, P, S), Previous);
}

TEST(BinaryDiff, SameSizeConstantEditStaysVisible) {
  std::vector<Fn> Old = sample(), New = sample();
  for (Fn &F : New)
    if (F.Name == "check2")
      F = diamond("check2", 58, 13); // was 12
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  const FunctionMatch &M = matchOf(R, P, "check2");
  EXPECT_EQ(counterpart(R, P, S, "check2"), "check2");
  EXPECT_TRUE(M.Changes & ChangeConstants) << changeLetters(M.Changes);
  EXPECT_FALSE(M.Changes & ChangeBlocks);
  EXPECT_FALSE(M.Changes & ChangeEdges);
  EXPECT_FALSE(M.identical());
  EXPECT_LT(M.Similarity, 1.0);
  // The aligned listing names the changed instruction.
  std::vector<uint32_t> PMap(P.Functions.size(), NoIndex),
      SMap(S.Functions.size(), NoIndex);
  for (const FunctionMatch &Pair : R.Matches) {
    PMap[Pair.Primary] = Pair.Secondary;
    SMap[Pair.Secondary] = Pair.Primary;
  }
  const FunctionDelta D =
      compareFunctions(P, M.Primary, S, M.Secondary, {PMap, SMap});
  unsigned Constants = 0;
  for (const DeltaRow &Row : D.Rows)
    if (Row.Kind == RowKind::Constant) {
      ++Constants;
      EXPECT_EQ(P.mnemonic(P.Instructions[Row.Primary]), "cmp");
    }
  EXPECT_EQ(Constants, 1u);
}

TEST(BinaryDiff, BranchInversionStaysVisible) {
  std::vector<Fn> Old = sample(), New = sample();
  for (Fn &F : New)
    if (F.Name == "check1")
      F = diamond("check1", 54, 11, /*Inverted=*/true);
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  EXPECT_EQ(counterpart(R, P, S, "check1"), "check1");
  const FunctionMatch &M = matchOf(R, P, "check1");
  EXPECT_TRUE(M.Changes & ChangeBranchInversion) << changeLetters(M.Changes);
  EXPECT_FALSE(M.identical());
}

TEST(BinaryDiff, AddedAndRemovedFunctionsStayUnmatched) {
  std::vector<Fn> Old = sample(), New = sample();
  Old.push_back(leaf("removed_only", 600, 9));
  New.push_back(diamond("added_only", 700, 3));
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  EXPECT_EQ(R.matchOfPrimary(indexOf(P, "removed_only")), nullptr);
  EXPECT_EQ(R.matchOfSecondary(indexOf(S, "added_only")), nullptr);
  EXPECT_EQ(R.PrimaryCoverage.Unmatched, 1u);
  EXPECT_EQ(R.SecondaryCoverage.Unmatched, 1u);
}

TEST(BinaryDiff, ChangedCalleesAreReported) {
  std::vector<Fn> Old = sample(), New = sample();
  for (Fn &F : New)
    if (F.Name == "release")
      F = caller("release", 95, {"free", "leaf4"}); // was leaf2
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  const FunctionMatch &M = matchOf(R, P, "release");
  EXPECT_EQ(counterpart(R, P, S, "release"), "release");
  EXPECT_TRUE(M.Changes & ChangeCalls) << changeLetters(M.Changes);
}

TEST(BinaryDiff, UnknownTargetsAreNotObservedChanges) {
  for (auto Flow : {InstructionFlow::Call, InstructionFlow::Jump,
                    InstructionFlow::Branch}) {
    ProgramBuilder Builder({});
    Builder.beginFunction(0x1000, 1, "caller", NameSource::Symbol, 0);
    Builder.addBlock(0x1000);
    Builder.addInstruction(0x1000, {0xe8}, "transfer", 1, 1, Flow, 0x2000);
    const auto P = Builder.finish();
    const auto R = compare(P, P, {});
    ASSERT_EQ(R.Matches.size(), 1u);
    const auto &M = R.Matches.front();
    EXPECT_TRUE(M.AnalysisComplete);
    EXPECT_FALSE(M.ReferenceComparisonComplete);
    EXPECT_FALSE(M.identical());
    EXPECT_EQ(M.Changes, 0u);
    EXPECT_EQ(changeState(M), "uncertain");
    EXPECT_LT(M.Similarity, 1);
    const std::vector<uint32_t> Map{0};
    const auto D = compareFunctions(P, 0, P, 0, {Map, Map});
    ASSERT_EQ(D.Rows.size(), 1u);
    EXPECT_EQ(D.Rows[0].Kind, RowKind::Uncertain);
    ASSERT_EQ(D.Blocks.size(), 1u);
    EXPECT_TRUE(D.Blocks[0].Uncertain);
    EXPECT_FALSE(D.Blocks[0].Unchanged);
    const auto Facts = observeFunctionChanges(P, 0, P, 0, D);
    EXPECT_TRUE(Facts.Observations.empty());
    EXPECT_FALSE(Facts.ReferenceComparisonComplete);
  }
}

TEST(BinaryDiff, UnknownDataDoesNotHideIndependentConstantChanges) {
  const auto Input = [](bool OtherScope, bool ChangedConstant) {
    ProgramBuilder B({});
    B.beginFunction(0x1000, 2, "load_and_add", NameSource::Symbol, 0);
    B.addBlock(0x1000);
    B.addInstruction(0x1000, {0x8b}, "load", OtherScope ? 11 : 10, 5,
                     InstructionFlow::None, {}, {}, {}, {}, 7, true);
    B.addReference(ReferenceKind::UnresolvedData, OtherScope ? 101 : 100, 0);
    const uint8_t Value = ChangedConstant ? 2 : 1;
    B.addInstruction(0x1001, {Value}, "add", Value, 3, InstructionFlow::None);
    return B.finish();
  };
  const auto P = Input(false, false);
  for (bool Changed : {false, true}) {
    const auto S = Input(true, Changed);
    const auto R = compare(P, S, {});
    ASSERT_EQ(R.Matches.size(), 1u);
    const auto &M = R.Matches.front();
    EXPECT_FALSE(M.ReferenceComparisonComplete);
    EXPECT_FALSE(M.identical());
    EXPECT_FALSE(M.Changes & ChangeReferences);
    EXPECT_EQ(bool(M.Changes & ChangeConstants), Changed);
    EXPECT_EQ(bool(M.Changes & ChangeBytes), Changed);
    EXPECT_EQ(changeState(M), Changed ? "changed" : "uncertain");
  }
}

TEST(BinaryDiff, UnknownAddressOperandDoesNotHideAKnownRetargeting) {
  const auto Input = [](bool Retarget) {
    ProgramBuilder B({});
    B.beginFunction(0x1000, 1, "holder", NameSource::Symbol, 0);
    B.addBlock(0x1000);
    const AddressReference Addresses[]{{Retarget ? 0x3000U : 0x2000U, 0},
                                       {0x4000, 1}};
    B.addInstruction(0x1000, {0x90}, "two_addresses", 1, 1,
                     InstructionFlow::None, {}, {}, {}, Addresses);
    for (uint64_t Address : {0x2000, 0x3000, 0x4000}) {
      B.beginFunction(Address, 1, "target", NameSource::Symbol, 0);
      B.addBlock(Address);
      B.addInstruction(Address, {0xc3}, "ret", 2, 2, InstructionFlow::Return);
    }
    return B.finish();
  };
  const auto P = Input(false), S = Input(true);
  const std::vector<uint32_t> Map{0, 1, 2, NoIndex};
  const auto D = compareFunctions(P, 0, S, 0, {Map, Map});
  EXPECT_FALSE(D.ReferenceComparisonComplete);
  EXPECT_TRUE(D.Changes & ChangeOperands);
  EXPECT_TRUE(D.Changes & ChangeReferences);
  EXPECT_FALSE(D.Changes & ChangeBytes);
  ASSERT_EQ(D.Rows.size(), 1u);
  EXPECT_EQ(D.Rows[0].Kind, RowKind::Operand);
  EXPECT_FALSE(D.Blocks[0].Uncertain);
}

TEST(BinaryDiff, UnknownAddressOperandPreservesOtherKnownReferenceChanges) {
  const auto Input = [](ReferenceKind Kind, bool OtherScope, bool Change,
                        bool Unknown) {
    ProgramBuilder B({});
    B.beginFunction(0x1000, 1, "holder", NameSource::Symbol, 0);
    B.addBlock(0x1000);
    const AddressReference Addresses[]{
        {OtherScope ? 101U : 100U, 0, ReferenceKind::UnresolvedData},
        {Change ? 201U : 200U, 1,
         Unknown ? ReferenceKind::UnresolvedData : Kind}};
    B.addInstruction(0x1000, {0x90}, "two_addresses", OtherScope ? 11 : 10, 5,
                     InstructionFlow::None, {}, {}, {}, Addresses, 7, true);
    return B.finish();
  };
  for (auto Kind : {ReferenceKind::String, ReferenceKind::Data}) {
    const auto P = Input(Kind, false, false, false);
    for (bool Changed : {false, true})
      for (bool Unknown : {false, true}) {
        const auto S = Input(Kind, true, Changed, Unknown);
        for (bool Reverse : {false, true}) {
          const auto D =
              compareFunctions(Reverse ? S : P, 0, Reverse ? P : S, 0, {});
          EXPECT_FALSE(D.ReferenceComparisonComplete);
          EXPECT_EQ(bool(D.Changes & ChangeReferences), Changed && !Unknown);
          EXPECT_EQ(bool(D.Changes & ChangeOperands), Changed && !Unknown);
          EXPECT_FALSE(D.Changes & ChangeBytes);
          ASSERT_EQ(D.Rows.size(), 1u);
          EXPECT_EQ(D.Rows[0].Kind, Changed && !Unknown ? RowKind::Operand
                                                        : RowKind::Uncertain);
        }
      }
  }
}

TEST(BinaryDiff, MappingEditsRemeasureAddressDependentsAndRestoreHistory) {
  auto Holder = leaf("holder", 3);
  Holder.AddressTargets = {"target"};
  const auto P = build({Holder, leaf("target", 5)}, 0x1000);
  const auto S = build({Holder, leaf("target", 5)}, 0x8000);
  auto R = compare(P, S, {});
  ASSERT_TRUE(matchOf(R, P, "holder").identical());
  const auto Original = saved(R, P, S);
  OverrideEffect Effect;
  const Override Decision{OverrideKind::Unmatch, named(P, "target").Address,
                          named(S, "target").Address};
  for (unsigned Retry = 0; Retry < 2; ++Retry) {
    ASSERT_FALSE(applyOverride(R, P, S, Decision, &Effect));
    EXPECT_FALSE(matchOf(R, P, "holder").ReferenceComparisonComplete);
    EXPECT_EQ(changeState(matchOf(R, P, "holder")), "uncertain");
    std::string Error;
    const auto Read = readComparison(saved(R, P, S), Error);
    ASSERT_TRUE(Read) << Error;
    ComparisonResult Bound;
    ASSERT_FALSE(bindComparison(*Read, P, S, Bound));
    EXPECT_EQ(changeState(matchOf(Bound, P, "holder")), "uncertain");
    revertOverride(R, P, S, Effect);
    EXPECT_TRUE(matchOf(R, P, "holder").identical());
    EXPECT_EQ(saved(R, P, S), Original);
  }
}

TEST(BinaryDiff, ChangedBodiesPairBySimilarityWhenStripped) {
  // Without names, an edited function still pairs through its callers or
  // its similarity, and its edit shows.
  std::vector<Fn> Old = sample(), New = sample();
  for (Fn &F : New)
    if (F.Name == "leaf5") {
      F.Blocks[0].insert(F.Blocks[0].begin() + 2, {"xor", "r3, r3"});
      F.Blocks[0][4].Operands = "r4, 999";
    }
  const Program P = build(stripped(Old), 0x400000);
  const Program S = build(stripped(New), 0x400000);
  const ComparisonResult R = compare(P, S, {});
  EXPECT_EQ(counterpart(R, P, S, "sub_leaf5"), "sub_leaf5");
  const FunctionMatch &M = matchOf(R, P, "sub_leaf5");
  EXPECT_TRUE(M.Changes & ChangeInstructions) << changeLetters(M.Changes);
  EXPECT_LT(M.Similarity, 1.0);
}

TEST(BinaryDiff, SharedGraphFeaturesRecoverCandidatesBeyondCommonSketches) {
  // Every function has the same instruction skeleton. Its sketch bucket is
  // deliberately too common, and the complete corpus exceeds exhaustive
  // admission. A small mnemonic/CFG group must still propose its alternatives.
  for (bool Duplicates : {false, true}) {
    std::vector<Fn> Before, After;
    for (unsigned Shape = 0; Shape != 65; ++Shape)
      for (unsigned Variant = 0; Variant != 4; ++Variant) {
        Fn F;
        F.Name =
            "family_" + std::to_string(Shape) + "_" + std::to_string(Variant);
        F.Source = NameSource::Generated;
        const unsigned Value = 200000 + Shape * 100 +
                               (Duplicates && Variant == 1 ? 0 : Variant) * 20;
        const unsigned Blocks = Shape + 3;
        for (unsigned Block = 0; Block != Blocks; ++Block) {
          F.Blocks.push_back(
              {{"mov", "r0, " + std::to_string(Value + Block)},
               {"xor", "r1, " + std::to_string(Value + Block + 8)},
               {Block + 1 == Blocks ? "ret" : "jmp", "",
                Block + 1 == Blocks ? InstructionFlow::Return
                                    : InstructionFlow::Jump}});
          if (Block + 1 != Blocks)
            F.Edges.emplace_back(Block, Block + 1, EdgeKind::Jump);
        }
        Before.push_back(F);
        F.Blocks.back()[1].Operands =
            "r1, " + std::to_string(Value + Blocks + 8);
        After.push_back(std::move(F));
      }
    std::vector<size_t> Layout;
    for (size_t I = After.size(); I != 0; --I)
      Layout.push_back(I - 1);
    const auto A = build(Before, 0x400000), B = build(After, 0x800000, Layout);
    for (bool Reverse : {false, true}) {
      const auto &P = Reverse ? B : A, &S = Reverse ? A : B;
      ComparisonSettings Settings;
      Settings.UseNames = false;
      const auto R = compare(P, S, Settings);
      ASSERT_EQ(R.Matches.size(), Duplicates ? 130U : 260U);
      for (unsigned Shape = 0; Shape != 65; ++Shape)
        for (unsigned Variant = 0; Variant != 4; ++Variant) {
          const auto Name =
              "family_" + std::to_string(Shape) + "_" + std::to_string(Variant);
          SCOPED_TRACE(Name);
          if (Duplicates && Variant < 2)
            EXPECT_EQ(R.matchOfPrimary(indexOf(P, Name)), nullptr);
          else
            EXPECT_EQ(counterpart(R, P, S, Name), Name);
        }
    }
  }
}

TEST(BinaryDiff, ShortLayoutEditsRetainCandidatesBeyondRunSketches) {
  Fn Before;
  Before.Name = "limit_check";
  Before.Source = NameSource::Generated;
  Before.Blocks = {{{"endbr64", ""},
                    {"cmp", "dword ptr [limit], 1"},
                    {"mov", "eax, 1"},
                    {"jbe", "exit", InstructionFlow::Branch}},
                   {{"ret", "", InstructionFlow::Return}},
                   {{"nop", "dword ptr [rax]"}},
                   {{"movzx", "eax, byte ptr [enabled]"},
                    {"ret", "", InstructionFlow::Return}}};
  Before.Edges = {{0, 3, EdgeKind::Taken},
                  {0, 1, EdgeKind::NotTaken},
                  {2, 3, EdgeKind::Fallthrough}};
  Fn After = Before;
  After.Blocks[0].erase(After.Blocks[0].begin());
  After.Blocks[0].insert(After.Blocks[0].end() - 1, Before.Blocks[2][0]);
  After.Blocks.erase(After.Blocks.begin() + 2);
  After.Edges = {{0, 2, EdgeKind::Taken}, {0, 1, EdgeKind::NotTaken}};
  Fn Decoy;
  Decoy.Name = "unrelated_check";
  Decoy.Source = NameSource::Generated;
  Decoy.Blocks = {{{"cmp", "edi, 3"}, {"jbe", "exit", InstructionFlow::Branch}},
                  {{"mov", "eax, 1"}, {"ret", "", InstructionFlow::Return}},
                  {{"xor", "eax, eax"}, {"ret", "", InstructionFlow::Return}}};
  Decoy.Edges = After.Edges;
  std::vector<Fn> Left{Before, Decoy}, Right{After};
  // A repeated background exceeds exhaustive admission and provides no unique
  // function identity. The edited check must enter through a bounded index.
  for (unsigned I = 0; I != 260; ++I) {
    Fn F;
    F.Name = "background_" + std::to_string(I);
    F.Source = NameSource::Generated;
    F.Blocks = {{{"div", "r8, r9"},
                 {"neg", "r10"},
                 {"ret", "", InstructionFlow::Return}}};
    Left.push_back(F);
    Right.push_back(F);
  }
  std::vector<size_t> Layout;
  for (size_t I = Right.size(); I != 0; --I)
    Layout.push_back(I - 1);
  const auto A = build(Left, 0x400000), B = build(Right, 0x800000, Layout);
  for (bool Reverse : {false, true}) {
    const auto &P = Reverse ? B : A, &S = Reverse ? A : B;
    const auto R = compare(P, S, {});
    EXPECT_EQ(counterpart(R, P, S, "limit_check"), "limit_check");
    EXPECT_EQ(R.Matches.size(), 1U);
    const auto *Matched = R.matchOfPrimary(indexOf(P, "limit_check"));
    ASSERT_NE(Matched, nullptr);
    EXPECT_TRUE(Matched->Changes & ChangeInstructions);
    EXPECT_LT(Matched->Similarity, 1.0);

    ComparisonSettings Exact;
    Exact.Approximate = false;
    EXPECT_EQ(compare(P, S, Exact).matchOfPrimary(indexOf(P, "limit_check")),
              nullptr);
    const Override Reject{OverrideKind::Unmatch,
                          P.Functions[indexOf(P, "limit_check")].Address,
                          S.Functions[indexOf(S, "limit_check")].Address};
    const auto Rejected = compare(P, S, {}, {Reject});
    const auto *Match = Rejected.matchOfPrimary(indexOf(P, "limit_check"));
    EXPECT_TRUE(!Match || Match->Secondary != indexOf(S, "limit_check"));
  }

  Fn Duplicate = After;
  Duplicate.Name = "duplicate_check";
  Right.push_back(std::move(Duplicate));
  const auto Duplicated = build(Right, 0x900000);
  for (bool Reverse : {false, true}) {
    const auto &P = Reverse ? Duplicated : A, &S = Reverse ? A : Duplicated;
    const auto R = compare(P, S, {});
    EXPECT_EQ(R.matchOfPrimary(indexOf(P, "limit_check")), nullptr);
    EXPECT_TRUE(R.Matches.empty());
  }
}

TEST(BinaryDiff, PartialInstructionPairsDoNotIdentifyUnrelatedBodies) {
  Fn Before;
  Before.Name = "original";
  Before.Source = NameSource::Generated;
  Before.Blocks = {{{"mov", "eax, ebx"},
                    {"add", "ecx, edx"},
                    {"xor", "esi, edi"},
                    {"or", "eax, ecx"},
                    {"and", "ebx, edx"},
                    {"sub", "edi, esi"},
                    {"rol", "eax, 1"},
                    {"ror", "ebx, 1"},
                    {"shl", "ecx, 1"},
                    {"shr", "edx, 1"},
                    {"neg", "esi"},
                    {"not", "edi"},
                    {"bswap", "eax"},
                    {"imul", "ebx, ecx"},
                    {"test", "edx, edx"},
                    {"cmp", "esi, edi"},
                    {"ret", "", InstructionFlow::Return}}};
  Fn After = Before;
  After.Name = "unrelated";
  for (unsigned I = 2; I < 16; I += 2)
    std::swap(After.Blocks[0][I], After.Blocks[0][I + 1]);
  After.Blocks[0][15] = {"inc", "edx"};
  // The initial pair and much of the instruction inventory survive, but no
  // complete run shingle, mnemonic sequence or multiset identifies this body.
  // Merely adding rare partial seeds would turn a similar CFG into an identity.
  std::vector<Fn> Left{Before}, Right{After};
  for (unsigned I = 0; I != 260; ++I) {
    Fn F;
    F.Name = "background_" + std::to_string(I);
    F.Source = NameSource::Generated;
    F.Blocks = {{{"div", "r8, r9"},
                 {"neg", "r10"},
                 {"ret", "", InstructionFlow::Return}}};
    Left.push_back(F);
    Right.push_back(F);
  }
  const auto A = build(Left, 0x400000), B = build(Right, 0x800000);
  for (bool Reverse : {false, true}) {
    const auto &P = Reverse ? B : A, &S = Reverse ? A : B;
    EXPECT_TRUE(compare(P, S, {}).Matches.empty());
  }
}

TEST(BinaryDiff, MovedStringEvidenceDoesNotConsumeTheSimilarBody) {
  Fn Old;
  Old.Name = "original";
  Old.Source = NameSource::Generated;
  Old.Strings = {"one diagnostic shared with a newly extracted helper"};
  Old.Blocks.resize(1);
  for (unsigned I = 0; I != 12; ++I)
    Old.Blocks[0].push_back(
        {I % 2 ? "xor" : "add", "r0, " + std::to_string(I + 10)});
  Old.Blocks[0].push_back({"ret", "", InstructionFlow::Return});
  Fn Same = Old;
  Same.Name = "changed_original";
  Same.Strings.clear();
  Same.Blocks[0].insert(Same.Blocks[0].begin() + 5, {"sub", "r0, 99"});
  Fn Helper;
  Helper.Name = "new_helper";
  Helper.Source = NameSource::Generated;
  Helper.Strings = Old.Strings;
  Helper.Blocks = {
      {{"div", "r6, r7"}, {"neg", "r8"}, {"ret", "", InstructionFlow::Return}}};
  for (bool WithCallSite : {false, true})
    for (bool Reverse : {false, true}) {
      std::vector<Fn> Before{Old}, After{Helper, Same};
      if (WithCallSite) {
        Before.push_back(caller("dispatch", 200, {"original"}));
        After.push_back(caller("dispatch", 200, {"new_helper"}));
      }
      const Program A = build(Before, 0x400000);
      const Program B = build(After, 0x500000);
      const Program &P = Reverse ? B : A, &S = Reverse ? A : B;
      const auto R = compare(P, S, {});
      EXPECT_EQ(counterpart(R, P, S, Reverse ? "changed_original" : "original"),
                Reverse ? "original" : "changed_original");
      EXPECT_EQ(R.Matches.size(), WithCallSite ? 2U : 1U);
      ComparisonSettings Exact;
      Exact.Approximate = false;
      const auto Uncorroborated = compare(P, S, Exact);
      for (const auto &Match : Uncorroborated.Matches)
        EXPECT_NE((Reverse ? P : S)
                      .Functions[Reverse ? Match.Primary : Match.Secondary]
                      .Name,
                  "new_helper");
    }
}

TEST(BinaryDiff, CallGraphDegreeCannotOverrideContradictingImportTargets) {
  Fn ReadImport, SeekImport;
  ReadImport.Name = "read";
  SeekImport.Name = "seek";
  ReadImport.Traits = SeekImport.Traits = TraitImport;
  Fn A;
  A.Name = "wrapper_a";
  A.Source = NameSource::Generated;
  A.Blocks = {{{"push", "r1"},
               {"call", "read", InstructionFlow::Call, "read"},
               {"ret", "", InstructionFlow::Return}}};
  Fn B = A;
  B.Name = "wrapper_b";
  B.Blocks[0][0] = {"mov", "r1, r2"};
  B.Blocks[0][1] = {"call", "seek", InstructionFlow::Call, "seek"};
  ComparisonSettings Settings;
  Settings.Approximate = false;
  const Program P = build({ReadImport, SeekImport, A}, 0x400000);
  const Program S = build({ReadImport, SeekImport, B}, 0x500000);
  const auto R = compare(P, S, Settings);
  EXPECT_EQ(R.Matches.size(), 2U);
  EXPECT_EQ(R.matchOfPrimary(indexOf(P, "wrapper_a")), nullptr);
}

TEST(BinaryDiff, InlinedTailPreludeKeepsWholeFunctionIdentityAmbiguous) {
  Fn Helper = leaf("helper", 19, 18);
  Helper.Source = NameSource::Generated;
  Fn Caller;
  Caller.Name = "caller";
  Caller.Source = NameSource::Generated;
  Caller.Blocks = {{{"store", "[state], r4"},
                    {"store", "[flag], 1"},
                    {"jmp", "", InstructionFlow::Jump, "helper"}}};
  Fn Merged = Helper;
  Merged.Name = "merged";
  Merged.Blocks[0].insert(Merged.Blocks[0].begin() + 2,
                          Caller.Blocks[0].begin(), Caller.Blocks[0].end() - 1);
  for (bool Reverse : {false, true})
    for (bool Reorder : {false, true})
      for (bool Approximate : {false, true}) {
        SCOPED_TRACE(::testing::Message() << Reverse << Reorder << Approximate);
        // A shared weak feature must not bypass the boundary check.
        Helper.Strings = Merged.Strings = {"output pipe"};
        const Program A = build({Caller, Helper}, 0x400000,
                                Reorder ? std::vector<size_t>{1, 0}
                                        : std::vector<size_t>{0, 1});
        const Program B = build({Merged}, 0x500000);
        const Program &P = Reverse ? B : A, &S = Reverse ? A : B;
        ComparisonSettings Settings;
        Settings.Approximate = Approximate;
        const auto R = compare(P, S, Settings);
        EXPECT_TRUE(R.Matches.empty());
        std::set<std::pair<std::string, std::string>> Candidates;
        for (const auto &Group : R.Ambiguities)
          for (uint32_t X : Group.Primary)
            for (uint32_t Y : Group.Secondary)
              Candidates.emplace(P.Functions[X].Name, S.Functions[Y].Name);
        const std::set<std::pair<std::string, std::string>> Expected =
            Reverse ? std::set<std::pair<std::string, std::string>>{{"merged",
                                                                     "helper"},
                                                                    {"merged",
                                                                     "caller"}}
                    : std::set<std::pair<std::string, std::string>>{
                          {"helper", "merged"}, {"caller", "merged"}};
        EXPECT_EQ(Candidates, Expected);
        std::string Error;
        const auto Read = readComparison(saved(R, P, S), Error);
        ASSERT_TRUE(bool(Read)) << Error;
        ComparisonResult Edited;
        ASSERT_FALSE(bindComparison(*Read, P, S, Edited));
        const auto Original = savedWithoutTimings(R, P, S);
        EXPECT_EQ(savedWithoutTimings(Edited, P, S), Original);
        OverrideEffect Effect;
        const Override Choice{
            OverrideKind::Match,
            P.Functions[indexOf(P, Reverse ? "merged" : "caller")].Address,
            S.Functions[indexOf(S, Reverse ? "caller" : "merged")].Address};
        ASSERT_FALSE(applyOverride(Edited, P, S, Choice, &Effect));
        EXPECT_EQ(counterpart(Edited, P, S, Reverse ? "merged" : "caller"),
                  Reverse ? "caller" : "merged");
        revertOverride(Edited, P, S, Effect);
        EXPECT_EQ(savedWithoutTimings(Edited, P, S), Original);
        const auto Forced = compare(P, S, Settings, {Choice});
        EXPECT_EQ(counterpart(Forced, P, S, Reverse ? "merged" : "caller"),
                  Reverse ? "caller" : "merged");
        Override Rejected = Choice;
        Rejected.Kind = OverrideKind::Unmatch;
        const auto WithoutCaller = compare(P, S, Settings, {Rejected});
        EXPECT_EQ(
            counterpart(WithoutCaller, P, S, Reverse ? "merged" : "helper"),
            Reverse ? "helper" : "merged");
      }
}

TEST(BinaryDiff, TailPreludeNeedsCompleteAndIndependentContribution) {
  Fn Helper = leaf("helper", 29, 18);
  Helper.Source = NameSource::Generated;
  Fn Caller;
  Caller.Name = "caller";
  Caller.Source = NameSource::Generated;
  Caller.Blocks = {{{"store", "[state], r4"},
                    {"store", "[flag], 1"},
                    {"jmp", "", InstructionFlow::Jump, "helper"}}};
  for (unsigned Variant = 0; Variant < 6; ++Variant) {
    SCOPED_TRACE(Variant);
    Fn C = Caller, Changed = Helper;
    Changed.Name = "changed_helper";
    if (Variant == 0) // Only part of the caller survives.
      Changed.Blocks[0].insert(Changed.Blocks[0].begin(), C.Blocks[0][0]);
    else if (Variant == 1) { // A changed operand is not the same prelude.
      Changed.Blocks[0].insert(Changed.Blocks[0].begin(), C.Blocks[0].begin(),
                               C.Blocks[0].end() - 1);
      Changed.Blocks[0][1].Operands = "[other_flag], 1";
    } else if (Variant == 2) { // Shared helper instructions add no evidence.
      C.Blocks[0][0] = Helper.Blocks[0][0];
      C.Blocks[0][1] = Helper.Blocks[0][1];
      Changed.Blocks[0].insert(Changed.Blocks[0].begin(), {"sub", "r3, 71"});
    } else { // A call or an unresolved jump is not an admitted tail edge.
      Changed.Blocks[0].insert(Changed.Blocks[0].begin(), C.Blocks[0].begin(),
                               C.Blocks[0].end() - 1);
      if (Variant == 3)
        C.Blocks[0].back().Flow = InstructionFlow::Call;
      else if (Variant == 4)
        C.Blocks[0].back().Callee.clear();
    }
    std::vector<Fn> After{Changed};
    if (Variant == 5) { // Independent identity already accounts for the caller.
      C.Source = NameSource::Symbol;
      Fn SurvivingCaller = C;
      SurvivingCaller.Blocks[0].back().Callee = "changed_helper";
      After.push_back(SurvivingCaller);
    }
    for (bool Reverse : {false, true}) {
      const Program A = build({C, Helper}, 0x400000);
      const Program B = build(After, 0x500000);
      const Program &P = Reverse ? B : A, &S = Reverse ? A : B;
      const auto R = compare(P, S, {});
      EXPECT_EQ(counterpart(R, P, S, Reverse ? "changed_helper" : "helper"),
                Reverse ? "helper" : "changed_helper");
    }
  }
}

TEST(BinaryDiff, EachInlinedTailCallerRetainsOnlyItsOwnMergedCandidate) {
  Fn Helper = leaf("helper", 39, 18);
  Helper.Source = NameSource::Generated;
  std::vector<Fn> Before{Helper}, After;
  for (unsigned I = 0; I < 2; ++I) {
    Fn Caller;
    Caller.Name = "caller" + std::to_string(I);
    Caller.Source = NameSource::Generated;
    Caller.Blocks = {{{"store", "[state" + std::to_string(I) + "], r4"},
                      {"store", "[flag" + std::to_string(I) + "], 1"},
                      {"jmp", "", InstructionFlow::Jump, "helper"}}};
    Fn Merged = Helper;
    Merged.Name = "merged" + std::to_string(I);
    Merged.Blocks[0].insert(Merged.Blocks[0].begin() + 2,
                            Caller.Blocks[0].begin(),
                            Caller.Blocks[0].end() - 1);
    Before.push_back(Caller);
    After.push_back(Merged);
  }
  for (bool Reverse : {false, true})
    for (bool Reorder : {false, true}) {
      const Program A = build(Before, 0x400000);
      const Program B = build(After, 0x500000,
                              Reorder ? std::vector<size_t>{1, 0}
                                      : std::vector<size_t>{0, 1});
      const Program &P = Reverse ? B : A, &S = Reverse ? A : B;
      const auto R = compare(P, S, {});
      EXPECT_TRUE(R.Matches.empty());
      std::set<std::pair<std::string, std::string>> Expected, Actual;
      for (unsigned I = 0; I < 2; ++I)
        for (const std::string &Source :
             {std::string("helper"), "caller" + std::to_string(I)}) {
          const std::string Target = "merged" + std::to_string(I);
          Expected.emplace(Reverse ? Target : Source,
                           Reverse ? Source : Target);
        }
      for (const auto &Group : R.Ambiguities)
        for (uint32_t X : Group.Primary)
          for (uint32_t Y : Group.Secondary)
            Actual.emplace(P.Functions[X].Name, S.Functions[Y].Name);
      EXPECT_EQ(Actual, Expected);
    }
}

TEST(BinaryDiff, ExactReferenceCandidatesStillCompeteWithoutApproximation) {
  Fn A = leaf("old", 12);
  A.Source = NameSource::Generated;
  A.Strings = {"unique input format diagnostic"};
  Fn B = A;
  B.Name = "new";
  B.Blocks[0].insert(B.Blocks[0].begin() + 2, {"sub", "r0, 99"});
  const Program P = build({A}, 0x400000), S = build({B}, 0x500000);
  ComparisonSettings Settings;
  Settings.Approximate = false;
  const auto R = compare(P, S, Settings);
  ASSERT_EQ(R.Matches.size(), 1U);
  EXPECT_EQ(R.Matches[0].Step, FunctionStep::StringReferences);
  EXPECT_LT(R.Matches[0].Similarity, 1.0);
}

TEST(BinaryDiff, AdditionalWeakEvidenceCannotSuppressMatchingCallContext) {
  Fn A = leaf("candidate", 17), B = leaf("candidate", 19);
  A.Source = B.Source = NameSource::Generated;
  B.Blocks[0].insert(B.Blocks[0].begin(), {"div", "r6, r7"});
  Fn Anchor;
  Anchor.Name = "anchor";
  Anchor.Blocks = {{{"call", "candidate", InstructionFlow::Call, "candidate"},
                    {"ret", "", InstructionFlow::Return}}};
  ComparisonSettings Settings;
  Settings.Approximate = false;
  Settings.MinimumSimilarity = 0.99;
  Settings.MinimumContextSimilarity = 0.01;
  for (bool AdditionalEvidence : {false, true}) {
    if (AdditionalEvidence)
      A.Strings = B.Strings = {"shared diagnostic"};
    const Program P = build({Anchor, A}, 0x400000);
    const Program S = build({Anchor, B}, 0x500000);
    const auto R = compare(P, S, Settings);
    ASSERT_EQ(R.Matches.size(), 2U);
    EXPECT_EQ(counterpart(R, P, S, "candidate"), "candidate");
  }
}

TEST(BinaryDiff,
     FunctionAddressReferencesFollowReorderedTargetsAndChainedAnchors) {
  Fn A = leaf("target_a", 101), B = leaf("target_b", 201);
  Fn X = leaf("pointer_a", 301), Y = X;
  Y.Name = "pointer_b";
  X.AddressTargets = {"target_a"};
  Y.AddressTargets = {"target_b"};
  Fn U = leaf("holder_a", 401), V = U;
  V.Name = "holder_b";
  U.AddressTargets = {"pointer_a"};
  V.AddressTargets = {"pointer_b"};
  for (Fn *F : {&A, &B, &X, &Y, &U, &V})
    F->Source = NameSource::Generated;
  const Program P = build({A, B, X, Y, U, V}, 0x400000);
  const Program S = build({V, Y, B, U, X, A}, 0x500000);
  for (bool Reverse : {false, true}) {
    const Program &Before = Reverse ? S : P, &After = Reverse ? P : S;
    const auto R = compare(Before, After, {});
    ASSERT_EQ(R.Matches.size(), 6U);
    for (const auto &M : R.Matches) {
      EXPECT_EQ(Before.Functions[M.Primary].Name,
                After.Functions[M.Secondary].Name);
      EXPECT_FALSE(M.Changes & ChangeReferences);
    }
  }
  // Without target correspondence, even identical native addresses do not
  // establish reference identity in two different programs.
  const auto Unknown = compareFunctions(P, indexOf(P, "pointer_a"), S,
                                        indexOf(S, "pointer_b"), {});
  EXPECT_FALSE(Unknown.Changes & ChangeReferences);
  EXPECT_FALSE(Unknown.ReferenceComparisonComplete);
  EXPECT_LT(Unknown.Similarity, 1.0);
}

TEST(BinaryDiff, FunctionAddressChainsPropagateBeyondGlobalPassRounds) {
  std::vector<Fn> Functions{leaf("anchor", 101)};
  for (unsigned I = 1; I < 512; ++I) {
    Fn F = leaf("pointer_" + std::to_string(I), 301);
    F.AddressTargets = {Functions.back().Name};
    Functions.push_back(std::move(F));
  }
  for (Fn &F : Functions)
    F.Source = NameSource::Generated;
  std::vector<size_t> Reversed;
  for (size_t I = Functions.size(); I != 0; --I)
    Reversed.push_back(I - 1);
  const Program P = build(Functions, 0x400000);
  const Program S = build(Functions, 0x500000, Reversed);
  ComparisonSettings Settings;
  Settings.Approximate = false;
  for (bool Reverse : {false, true}) {
    const Program &Before = Reverse ? S : P, &After = Reverse ? P : S;
    const auto R = compare(Before, After, Settings);
    ASSERT_EQ(R.Matches.size(), Functions.size());
    for (const auto &M : R.Matches)
      EXPECT_EQ(Before.Functions[M.Primary].Name,
                After.Functions[M.Secondary].Name);
  }
}

TEST(BinaryDiff, CandidateAllowanceCanReachPastFourQuickScoreDistractors) {
  const auto Make = [](std::string Name, unsigned Base, bool Mixed) {
    Fn F;
    F.Name = std::move(Name);
    F.Blocks.resize(1);
    for (unsigned I = 0; I < 6; ++I)
      F.Blocks[0].push_back(
          {"mov", std::string(Mixed && I % 3 == 1 ? "v0, " : "r0, ") +
                      std::to_string(Base + I)});
    F.Blocks[0].push_back({"ret", "", InstructionFlow::Return});
    return F;
  };
  // Changing two operand families breaks all three-instruction sketch runs,
  // while four original instructions and the return remain exact. Decoys
  // retain the other side's skeleton sequence but change every constant.
  std::vector<Fn> Left{Make("target", 100000, false)};
  std::vector<Fn> Right{Make("target", 100000, true)};
  for (unsigned I = 0; I < 5; ++I) {
    Left.push_back(Make("left" + std::to_string(I), 300000 + 100 * I, true));
    Right.push_back(Make("right" + std::to_string(I), 400000 + 100 * I, false));
  }
  const auto P = build(Left, 0x400000);
  const auto S = build(Right, 0x500000);
  ComparisonSettings Settings;
  Settings.UseNames = false;
  Settings.MaxCandidates = 4;
  EXPECT_EQ(compare(P, S, Settings).secondaryOf(0), NoIndex);
  for (const unsigned Allowance : {6u, 16u, 256u}) {
    SCOPED_TRACE(Allowance);
    Settings.MaxCandidates = Allowance;
    const auto R = compare(P, S, Settings);
    ASSERT_EQ(R.secondaryOf(0), 0u);
    const auto *M = R.matchOfPrimary(0);
    ASSERT_TRUE(M);
    EXPECT_EQ(M->Step, FunctionStep::Similarity);
    EXPECT_EQ(M->EqualInstructions, 5u);
    EXPECT_TRUE(M->Changes & ChangeOperands);
    // Indistinguishable distractors remain available for analyst review.
    EXPECT_EQ(R.Matches.size(), 1u);
    EXPECT_FALSE(R.Ambiguities.empty());
  }
}

TEST(BinaryDiff, SimilarStructureWithOtherInstructionsIsNotIdentical) {
  std::vector<Fn> Old = sample(), New = sample();
  for (Fn &F : New)
    if (F.Name == "check3")
      for (auto &Block : F.Blocks)
        for (Op &O : Block)
          if (O.Mnemonic == "mov")
            O.Mnemonic = "sub";
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  const FunctionMatch &M = matchOf(R, P, "check3");
  EXPECT_FALSE(M.identical());
  EXPECT_LT(M.Similarity, 1.0);
  EXPECT_TRUE(M.Changes & ChangeInstructions);
}

TEST(BinaryDiff, ResultsDoNotDependOnThreadCount) {
  std::vector<Fn> Old = sample(), New = sample();
  Fn Helper = leaf("tail_helper", 1200, 25), Prelude;
  Prelude.Name = "tail_caller";
  Prelude.Blocks = {{{"store", "[state], r4"},
                     {"store", "[flag], 1"},
                     {"jmp", "", InstructionFlow::Jump, "tail_helper"}}};
  Old.push_back(Prelude);
  Old.push_back(Helper);
  Helper.Name = "tail_merged";
  Helper.Blocks[0].insert(Helper.Blocks[0].begin() + 2,
                          Prelude.Blocks[0].begin(),
                          Prelude.Blocks[0].end() - 1);
  New.push_back(Helper);
  for (unsigned I = 0; I < 40; ++I) {
    Old.push_back(leaf("bulk" + std::to_string(I), 1000 + I, 4 + I % 5));
    New.push_back(
        leaf("bulk" + std::to_string(I), 1000 + I + (I % 7 == 0), 4 + I % 5));
  }
  const Program P = build(stripped(Old), 0x400000);
  const Program S = build(stripped(New), 0x900000);
  std::string Reference;
  for (unsigned Threads : {1u, 2u, 8u}) {
    neverd::setWorkerThreadCount(Threads);
    const std::string Text = savedWithoutTimings(compare(P, S, {}), P, S);
    if (Reference.empty())
      Reference = Text;
    EXPECT_EQ(Text, Reference) << Threads << " threads";
  }
  neverd::setWorkerThreadCount(0);
}

TEST(BinaryDiff, SavedComparisonsRoundTripAndRefuseOtherInputs) {
  std::vector<Fn> Old = sample(), New = sample();
  for (Fn &F : New)
    if (F.Name == "check2")
      F = diamond("check2", 58, 13);
  const Program P = build(Old, 0x400000, {}, std::string(64, 'b'));
  const Program S = build(New, 0x400000, {}, std::string(64, 'c'));
  ComparisonResult R = compare(P, S, {});
  ASSERT_FALSE(applyOverride(R, P, S,
                             {OverrideKind::Confirm, named(P, "leaf0").Address,
                              named(S, "leaf0").Address}));
  const std::string Text = saved(R, P, S);

  std::string Error;
  const auto Read = readComparison(Text, Error);
  ASSERT_TRUE(Read) << Error;
  ComparisonResult Bound;
  ASSERT_FALSE(bindComparison(*Read, P, S, Bound));
  EXPECT_EQ(savedWithoutTimings(Bound, P, S), savedWithoutTimings(R, P, S));
  EXPECT_EQ(Bound.Overrides.size(), 1u);
  EXPECT_TRUE(matchOf(Bound, P, "leaf0").Confirmed);

  // Another input, another analysis, or a function the analysis lost.
  BindFailure Why;
  const Program Other = build(Old, 0x400000, {}, std::string(64, 'd'));
  EXPECT_TRUE(bindComparison(*Read, Other, S, Bound, &Why));
  EXPECT_EQ(Why, BindFailure::StaleInput);
  const Program Reanalyzed =
      build(Old, 0x400000, {}, std::string(64, 'b'), "other settings");
  const auto SettingsError = bindComparison(*Read, Reanalyzed, S, Bound, &Why);
  ASSERT_TRUE(SettingsError);
  EXPECT_NE(SettingsError->find("other settings"), std::string::npos);
  EXPECT_EQ(Why, BindFailure::StaleAnalysis);
  auto Recovered = P;
  Recovered.Identity.Revision = "newly-discovered-functions";
  const auto SnapshotError = bindComparison(*Read, Recovered, S, Bound, &Why);
  ASSERT_TRUE(SnapshotError);
  EXPECT_NE(SnapshotError->find("analyzed snapshot"), std::string::npos);
  EXPECT_EQ(SnapshotError->find("other settings"), std::string::npos);
  EXPECT_EQ(Why, BindFailure::StaleAnalysis);
  EXPECT_EQ(savedWithoutTimings(Bound, P, S), savedWithoutTimings(R, P, S));
  std::vector<Fn> Fewer = Old;
  llvm::erase_if(Fewer, [](const Fn &F) { return F.Name == "leaf3"; });
  const Program Lost = build(Fewer, 0x400000, {}, std::string(64, 'b'));
  EXPECT_TRUE(bindComparison(*Read, Lost, S, Bound, &Why));
}

TEST(BinaryDiff, MalformedSavedComparisonsFailClearly) {
  const Program P = build(sample(), 0x400000);
  const Program S = build(sample(), 0x400000);
  const std::string Good = saved(compare(P, S, {}), P, S);
  std::string Error;
  ASSERT_TRUE(readComparison(Good, Error)) << Error;
  const auto Rejects = [&](std::string Text, llvm::StringRef Expected) {
    std::string E;
    EXPECT_FALSE(readComparison(Text, E));
    EXPECT_NE(E.find(Expected.str()), std::string::npos)
        << "error: " << E << "\nexpected: " << Expected.str();
  };
  const auto Replace = [&](llvm::StringRef From, llvm::StringRef To) {
    std::string Text = Good;
    const size_t At = Text.find(From.str());
    EXPECT_NE(At, std::string::npos) << From.str();
    if (At != std::string::npos)
      Text.replace(At, From.size(), To.str());
    return Text;
  };
  Rejects("", "not JSON");
  Rejects("[]", "must be a JSON object");
  Rejects("{\"schema\":\"other\"}", "not a saved comparison");
  Rejects(Replace("\"schema_version\":4", "\"schema_version\":99"),
          "version 99 is not supported");
  Rejects(Replace("\"state\":\"complete\"", "\"state\":\"maybe\""),
          "\"state\"");
  Rejects(Replace("\"step\":\"", "\"step\":\"bogus_"), "unknown matching step");
  Rejects(Replace("\"similarity\":1", "\"similarity\":7"), "\"similarity\"");
  Rejects(Replace("\"max_candidates\":16", "\"max_candidates\":0"),
          "candidates");
  Rejects(Replace("\"primary\":\"0x", "\"primary\":\"0xZZ"), "hexadecimal");
  Rejects(Replace("\"sha256\":\"aaaa", "\"sha256\":\"zzzz"), "sha256");
  Rejects(
      Replace("\"functions\":{\"primary\":[", "\"functions\":{\"primary\":[5,"),
      "a function must be");
}

TEST(BinaryDiff, CandidateOnlyStepsCannotPublishAcceptedMatches) {
  const Program P = build({leaf("body", 3, 5)}, 0x400000);
  const Program S = build({leaf("body", 3, 5)}, 0x800000);
  const ComparisonResult Original = compare(P, S, {});
  ASSERT_EQ(Original.Matches.size(), 1u);
  const std::string Before = saved(Original, P, S);
  std::string Error;
  const auto Parsed = readComparison(Before, Error);
  ASSERT_TRUE(Parsed) << Error;
  for (bool Confirmed : {false, true}) {
    SCOPED_TRACE(Confirmed);
    auto Invalid = Original;
    Invalid.Matches.front().Step = FunctionStep::InlinedTailCall;
    Invalid.Matches.front().Confirmed = Confirmed;
    EXPECT_FALSE(readComparison(saved(Invalid, P, S), Error));
    EXPECT_NE(Error.find("can only describe ambiguous candidates"),
              std::string::npos)
        << Error;

    // Typed saved state uses the same validator and cannot replace a result
    // already published to its caller when the evidence is invalid.
    auto Mutated = *Parsed;
    Mutated.Matches.front().Match.Step = FunctionStep::InlinedTailCall;
    Mutated.Matches.front().Match.Confirmed = Confirmed;
    auto Bound = Original;
    BindFailure Why;
    EXPECT_TRUE(bindComparison(Mutated, P, S, Bound, &Why));
    EXPECT_EQ(Why, BindFailure::InvalidEvidence);
    EXPECT_EQ(saved(Bound, P, S), Before);
  }
}

TEST(BinaryDiff, OverridesKeepPairsOneToOneAndRevert) {
  const Program P = build(sample(), 0x400000);
  const Program S = build(sample(), 0x400000);
  ComparisonResult R = compare(P, S, {});
  const std::string Before = savedWithoutTimings(R, P, S);
  // Pair leaf0 with leaf1's counterpart: both former partners come free.
  OverrideEffect Effect;
  ASSERT_FALSE(applyOverride(R, P, S,
                             {OverrideKind::Match, named(P, "leaf0").Address,
                              named(S, "leaf1").Address},
                             &Effect));
  EXPECT_EQ(counterpart(R, P, S, "leaf0"), "leaf1");
  EXPECT_EQ(R.matchOfPrimary(indexOf(P, "leaf1")), nullptr);
  EXPECT_EQ(R.matchOfSecondary(indexOf(S, "leaf0")), nullptr);
  EXPECT_EQ(matchOf(R, P, "leaf0").Step, FunctionStep::Manual);
  EXPECT_TRUE(matchOf(R, P, "leaf0").Confirmed);
  EXPECT_EQ(R.Overrides.size(), 1u);
  revertOverride(R, P, S, Effect);
  EXPECT_EQ(savedWithoutTimings(R, P, S), Before);

  // Unpairing and confirming need an existing pair.
  EXPECT_TRUE(applyOverride(R, P, S,
                            {OverrideKind::Unmatch, named(P, "leaf0").Address,
                             named(S, "leaf1").Address}));
  EXPECT_TRUE(applyOverride(
      R, P, S, {OverrideKind::Confirm, named(P, "leaf0").Address, 0x1234}));
  ASSERT_FALSE(applyOverride(R, P, S,
                             {OverrideKind::Unmatch, named(P, "leaf2").Address,
                              named(S, "leaf2").Address},
                             &Effect));
  EXPECT_EQ(R.matchOfPrimary(indexOf(P, "leaf2")), nullptr);
  revertOverride(R, P, S, Effect);
  EXPECT_EQ(counterpart(R, P, S, "leaf2"), "leaf2");

  // Comparing again with the overrides reproduces them.
  const std::vector<Override> Decisions = {
      {OverrideKind::Match, named(P, "leaf3").Address,
       named(S, "leaf4").Address},
      {OverrideKind::Unmatch, named(P, "check0").Address,
       named(S, "check0").Address}};
  const ComparisonResult Again = compare(P, S, {}, Decisions);
  EXPECT_EQ(counterpart(Again, P, S, "leaf3"), "leaf4");
  EXPECT_NE(counterpart(Again, P, S, "check0"), "check0");
  EXPECT_EQ(Again.Overrides, Decisions);
}

TEST(BinaryDiff, ExhaustedAmbiguitiesStaySaveableAndRevertExactly) {
  for (const auto [PCount, SCount] :
       {std::pair(1u, 3u), std::pair(3u, 1u), std::pair(3u, 3u)}) {
    SCOPED_TRACE(PCount);
    SCOPED_TRACE(SCount);
    const auto Copies = [](unsigned Count) {
      std::vector<Fn> Fns;
      for (unsigned I = 0; I < Count; ++I)
        Fns.push_back(leaf("copy" + std::to_string(I), 7, 1));
      return Fns;
    };
    const Program P = build(Copies(PCount), 0x400000);
    const Program S = build(Copies(SCount), 0x800000);
    ComparisonSettings Settings;
    Settings.UseNames = false;
    auto R = compare(P, S, Settings);
    ASSERT_TRUE(R.Matches.empty());
    ASSERT_EQ(R.Ambiguities.size(), 1u);
    std::vector<OverrideEffect> Effects;
    std::vector<std::string> States{savedWithoutTimings(R, P, S)};
    const unsigned Pairs = std::min(PCount, SCount);
    for (unsigned I = 0; I < Pairs; ++I) {
      OverrideEffect E;
      ASSERT_FALSE(applyOverride(
          R, P, S,
          {OverrideKind::Match, P.Functions[I].Address, S.Functions[I].Address},
          &E));
      Effects.push_back(std::move(E));
      const bool HasCandidates = I + 1 < Pairs;
      EXPECT_EQ(R.Ambiguities.size(), HasCandidates ? 1u : 0u);
      EXPECT_EQ(R.PrimaryCoverage.Ambiguous,
                HasCandidates ? PCount - I - 1 : 0u);
      EXPECT_EQ(R.SecondaryCoverage.Ambiguous,
                HasCandidates ? SCount - I - 1 : 0u);
      EXPECT_EQ(R.PrimaryCoverage.Unmatched,
                HasCandidates ? 0u : PCount - Pairs);
      EXPECT_EQ(R.SecondaryCoverage.Unmatched,
                HasCandidates ? 0u : SCount - Pairs);
      States.push_back(savedWithoutTimings(R, P, S));
      std::string Error;
      const auto Read = readComparison(States.back(), Error);
      ASSERT_TRUE(Read) << Error;
      ComparisonResult Reopened;
      ASSERT_FALSE(bindComparison(*Read, P, S, Reopened));
      EXPECT_EQ(savedWithoutTimings(Reopened, P, S), States.back());
    }
    for (unsigned I = Pairs; I > 0; --I) {
      revertOverride(R, P, S, Effects[I - 1]);
      EXPECT_EQ(savedWithoutTimings(R, P, S), States[I - 1]);
    }
    for (unsigned I = 0; I < Pairs; ++I) {
      ASSERT_FALSE(applyOverride(R, P, S, Effects[I].Applied));
      EXPECT_EQ(savedWithoutTimings(R, P, S), States[I + 1]);
    }
  }
}

TEST(BinaryDiff, CrossGroupOverridesRestoreRemovedAndRetainedAmbiguities) {
  std::vector<Fn> PFns, SFns;
  for (unsigned Group = 0; Group < 4; ++Group)
    for (unsigned Copy = 0; Copy < 2; ++Copy) {
      const auto F =
          leaf("group" + std::to_string(Group) + "_" + std::to_string(Copy),
               Group + 1, Group + 1);
      if (Group != 1 || Copy == 0)
        PFns.push_back(F);
      if (Group != 0 || Copy == 0)
        SFns.push_back(F);
    }
  const Program P = build(PFns, 0x400000);
  const Program S = build(SFns, 0x800000);
  ComparisonSettings Settings;
  Settings.UseNames = false;
  auto R = compare(P, S, Settings);
  ASSERT_TRUE(R.Matches.empty());
  ASSERT_EQ(R.Ambiguities.size(), 4u);
  // Each edit consumes a whole group and may also shrink a different group.
  // The untouched fourth group must keep its original order and membership.
  const std::pair<const char *, const char *> Pairs[] = {
      {"group2_0", "group0_0"},
      {"group1_0", "group2_0"},
      {"group2_1", "group2_1"}};
  std::vector<OverrideEffect> Effects;
  std::vector<std::string> States{savedWithoutTimings(R, P, S)};
  for (const auto &[A, B] : Pairs) {
    OverrideEffect E;
    ASSERT_FALSE(applyOverride(
        R, P, S,
        {OverrideKind::Match, named(P, A).Address, named(S, B).Address}, &E));
    Effects.push_back(std::move(E));
    EXPECT_EQ(R.Ambiguities.size(), 4u - Effects.size());
    States.push_back(savedWithoutTimings(R, P, S));
    std::string Error;
    const auto Read = readComparison(States.back(), Error);
    ASSERT_TRUE(Read) << Error;
    ComparisonResult Reopened;
    ASSERT_FALSE(bindComparison(*Read, P, S, Reopened));
    EXPECT_EQ(savedWithoutTimings(Reopened, P, S), States.back());
  }
  for (size_t I = Effects.size(); I > 0; --I) {
    revertOverride(R, P, S, Effects[I - 1]);
    EXPECT_EQ(savedWithoutTimings(R, P, S), States[I - 1]);
  }
  for (size_t I = 0; I < Effects.size(); ++I) {
    ASSERT_FALSE(applyOverride(R, P, S, Effects[I].Applied));
    EXPECT_EQ(savedWithoutTimings(R, P, S), States[I + 1]);
  }
  for (size_t I = Effects.size(); I > 0; --I)
    revertOverride(R, P, S, Effects[I - 1]);
  // A single cross-group edit can exhaust both original groups at once.
  OverrideEffect Both;
  ASSERT_FALSE(applyOverride(R, P, S,
                             {OverrideKind::Match, named(P, "group1_0").Address,
                              named(S, "group0_0").Address},
                             &Both));
  EXPECT_EQ(R.Ambiguities.size(), 2u);
  revertOverride(R, P, S, Both);
  EXPECT_EQ(savedWithoutTimings(R, P, S), States.front());
}

TEST(BinaryDiff, StoppedComparisonsKeepWhatTheyHave) {
  const Program P = build(sample(), 0x400000);
  const Program S = build(sample(), 0x400000);
  std::atomic<bool> Cancel{true};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  const ComparisonResult R = compare(P, S, {}, {}, Control);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_FALSE(R.StoppedPhase.empty());
  for (const FunctionMatch &M : R.Matches)
    EXPECT_EQ(P.Functions[M.Primary].Name, S.Functions[M.Secondary].Name);

  ComparisonControl Late;
  Late.Deadline = std::chrono::steady_clock::now();
  EXPECT_EQ(compare(P, S, {}, {}, Late).State, ComparisonState::TimedOut);

  std::vector<std::string> Phases;
  ComparisonControl Watch;
  Watch.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phases.empty() || Phases.back() != Phase)
      Phases.push_back(Phase.str());
  };
  EXPECT_EQ(compare(P, S, {}, {}, Watch).State, ComparisonState::Complete);
  EXPECT_FALSE(Phases.empty());
}

TEST(BinaryDiff, SavedMatchesCannotContradictEffectiveUnmatches) {
  const auto P = build({leaf("a", 1, 3), leaf("b", 2, 4)}, 0x400000);
  const auto S = build({leaf("a", 1, 3), leaf("b", 2, 4)}, 0x800000);
  auto Original = compare(P, S, {});
  const auto Before = saved(Original, P, S);
  std::string Error;
  const auto Read = readComparison(Before, Error);
  ASSERT_TRUE(Read) << Error;
  const auto A = named(P, "a").Address, B = named(S, "a").Address;
  for (const auto History :
       {std::vector<Override>{{OverrideKind::Unmatch, A, B}},
        std::vector<Override>{{OverrideKind::Unmatch, A, B},
                              {OverrideKind::Confirm, A, B}},
        std::vector<Override>{{OverrideKind::Match, A, B},
                              {OverrideKind::Unmatch, A, B}}}) {
    for (const auto State :
         {ComparisonState::Complete, ComparisonState::Cancelled,
          ComparisonState::TimedOut, ComparisonState::ResourceLimited}) {
      auto Forged = Original;
      Forged.Overrides = History;
      Forged.State = State;
      EXPECT_FALSE(readComparison(saved(Forged, P, S), Error));
      EXPECT_NE(Error.find("effective unmatch"), std::string::npos) << Error;

      auto Detached = *Read;
      Detached.Overrides = History;
      Detached.State = State;
      BindFailure Why;
      const auto Failure = bindComparison(Detached, P, S, Original, &Why);
      ASSERT_TRUE(Failure);
      EXPECT_NE(Failure->find("effective unmatch"), std::string::npos);
      EXPECT_EQ(Why, BindFailure::InvalidEvidence);
      EXPECT_EQ(saved(Original, P, S), Before);
    }
  }
}

TEST(BinaryDiff, SavedDecisionsRetainRemappingAndUndoSemantics) {
  const auto P = build({leaf("a", 1, 3), leaf("b", 2, 4)}, 0x400000);
  const auto S = build({leaf("a", 1, 3), leaf("b", 2, 4)}, 0x800000);
  const auto A = named(P, "a").Address, D = named(P, "b").Address;
  const auto B = named(S, "a").Address, C = named(S, "b").Address;
  const auto RoundTrip = [&](const ComparisonResult &Result) {
    const auto Text = saved(Result, P, S);
    std::string Error;
    const auto Read = readComparison(Text, Error);
    ASSERT_TRUE(Read) << Error;
    ComparisonResult Bound;
    ASSERT_FALSE(bindComparison(*Read, P, S, Bound));
    EXPECT_EQ(saved(Bound, P, S), Text);
  };
  for (const auto History :
       {std::vector<Override>{{OverrideKind::Unmatch, A, B},
                              {OverrideKind::Match, A, C},
                              {OverrideKind::Match, D, B}},
        std::vector<Override>{{OverrideKind::Unmatch, A, B},
                              {OverrideKind::Confirm, A, B}},
        std::vector<Override>{{OverrideKind::Unmatch, A, B},
                              {OverrideKind::Match, A, B},
                              {OverrideKind::Match, A, C}},
        std::vector<Override>{{OverrideKind::Unmatch, A, B},
                              {OverrideKind::Match, A, C},
                              {OverrideKind::Match, D, B},
                              {OverrideKind::Match, A, B}}}) {
    RoundTrip(compare(P, S, {}, History));
  }
  auto Result = compare(P, S, {});
  const auto Before = saved(Result, P, S);
  std::vector<OverrideEffect> Effects;
  for (const auto Decision : {Override{OverrideKind::Unmatch, A, B},
                              Override{OverrideKind::Match, A, B},
                              Override{OverrideKind::Match, A, C}}) {
    Effects.emplace_back();
    ASSERT_FALSE(applyOverride(Result, P, S, Decision, &Effects.back()));
    RoundTrip(Result);
  }
  while (!Effects.empty()) {
    revertOverride(Result, P, S, Effects.back());
    Effects.pop_back();
    RoundTrip(Result);
  }
  EXPECT_EQ(saved(Result, P, S), Before);
}

TEST(BinaryDiff, EffectiveRejectionsAgreeWithReverseHistoryOracle) {
  // Every sequence of five decisions on two pairs, including shared endpoints.
  constexpr unsigned Sequences = 6 * 6 * 6 * 6 * 6;
  const std::pair<uint64_t, uint64_t> Pairs[] = {{10, 20}, {10, 30}};
  for (unsigned Sequence = 0; Sequence < Sequences; ++Sequence) {
    unsigned Code = Sequence;
    std::vector<Override> History;
    for (unsigned I = 0; I < 5; ++I, Code /= 6) {
      const auto &Pair = Pairs[Code % 2];
      History.push_back(
          {static_cast<OverrideKind>((Code % 6) / 2), Pair.first, Pair.second});
    }
    std::vector<std::pair<uint64_t, uint64_t>> Expected;
    for (const auto &Pair : Pairs) {
      for (auto It = History.rbegin(); It != History.rend(); ++It) {
        if (It->Primary != Pair.first || It->Secondary != Pair.second ||
            It->Kind == OverrideKind::Confirm)
          continue;
        if (It->Kind == OverrideKind::Unmatch)
          Expected.push_back(Pair);
        break;
      }
    }
    EXPECT_EQ(rejectedPairs(History), Expected) << Sequence;
  }
  std::atomic<bool> Cancel = true;
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  EXPECT_THROW(rejectedPairs({}, Control), ComparisonInterrupted);
  Cancel = false;
  Control.Deadline = std::chrono::steady_clock::now();
  EXPECT_THROW(rejectedPairs({}, Control), ComparisonInterrupted);
}

TEST(BinaryDiff, LibraryCodeDoesNotDecideTheProgramScore) {
  std::vector<Fn> Old = sample(), New = sample();
  Fn Big = leaf("runtime_init", 5000, 400),
     BigNew = leaf("runtime_init", 6000, 400);
  Big.Traits = BigNew.Traits = TraitLibrary;
  Old.push_back(Big);
  New.push_back(BigNew);
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000);
  const ComparisonResult R = compare(P, S, {});
  EXPECT_EQ(R.Similarity, 1.0);
  EXPECT_EQ(R.PrimaryCoverage.Library, 1u);
}

TEST(BinaryDiff, ImportsPairBySymbol) {
  std::vector<Fn> Old = sample(), New = sample();
  New.push_back(import("strlen"));
  std::vector<size_t> Order;
  for (size_t I = New.size(); I-- > 0;)
    Order.push_back(I);
  const Program P = build(Old, 0x400000);
  const Program S = build(New, 0x400000, Order);
  const ComparisonResult R = compare(P, S, {});
  for (llvm::StringRef Name : {"malloc", "free", "memcpy"}) {
    EXPECT_EQ(counterpart(R, P, S, Name), Name.str());
    EXPECT_EQ(matchOf(R, P, Name).Step, FunctionStep::ImportName);
    EXPECT_TRUE(matchOf(R, P, Name).IdentityOnly);
    EXPECT_FALSE(matchOf(R, P, Name).identical());
  }
  EXPECT_EQ(R.matchOfSecondary(indexOf(S, "strlen")), nullptr);
}

TEST(BinaryDiff, BodilessPairsNeverClaimCodeEquivalence) {
  const auto P = build({import("external"), leaf("body", 3, 5)}, 0x400000);
  const auto S = build({import("external"), leaf("body", 3, 5)}, 0x800000);
  auto R = compare(P, S, {});
  auto D = compareFunctions(P, indexOf(P, "external"), S,
                            indexOf(S, "external"), {});
  EXPECT_TRUE(D.IdentityOnly);
  EXPECT_FALSE(D.AnalysisComplete);
  EXPECT_TRUE(D.Rows.empty());
  OverrideEffect Effect;
  ASSERT_FALSE(applyOverride(R, P, S,
                             {OverrideKind::Match, named(P, "external").Address,
                              named(S, "body").Address},
                             &Effect));
  const auto &M = matchOf(R, P, "external");
  EXPECT_TRUE(M.Measured);
  EXPECT_FALSE(M.IdentityOnly);
  EXPECT_FALSE(M.AnalysisComplete);
  EXPECT_FALSE(M.identical());
  EXPECT_NE(M.Changes & ~MovementChanges, 0u);
  EXPECT_EQ(M.EqualInstructions, 0u);
  EXPECT_LT(M.Similarity, 1.0);
  revertOverride(R, P, S, Effect);
  EXPECT_TRUE(matchOf(R, P, "external").IdentityOnly);
  EXPECT_TRUE(matchOf(R, P, "body").identical());
}

TEST(BinaryDiff, SavedMeasurementsAreRecomputedBeforePublication) {
  const auto P = build({leaf("body", 3, 5), import("external")}, 0x400000);
  const auto S = build({leaf("body", 7, 5), import("external")}, 0x800000);
  auto R = compare(P, S, {});
  const auto Text = saved(R, P, S);
  std::string Error;
  const auto Read = readComparison(Text, Error);
  ASSERT_TRUE(Read) << Error;
  const auto Rejected = [&](auto Mutate) {
    auto Forged = *Read;
    Mutate(Forged.Matches.front().Match);
    BindFailure Why;
    auto Failure = bindComparison(Forged, P, S, R, &Why);
    EXPECT_TRUE(Failure);
    EXPECT_EQ(Why, BindFailure::InvalidEvidence);
    EXPECT_EQ(saved(R, P, S), Text);
  };
  Rejected([](auto &M) { M.Changes = 0; });
  Rejected([](auto &M) { M.AnalysisComplete = false; });
  Rejected([](auto &M) { M.IdentityOnly = true; });
  Rejected([](auto &M) { M.Similarity = 1; });
  Rejected([](auto &M) { ++M.MatchedBlocks; });
  Rejected([](auto &M) { ++M.MatchedEdges; });
  Rejected([](auto &M) { ++M.EqualInstructions; });
  Rejected([](auto &M) { M.Measured = false; });
  // A stopped comparison may retain unmeasured correspondence, without claims.
  auto Partial = *Read;
  Partial.State = ComparisonState::Cancelled;
  auto &M = Partial.Matches.front().Match;
  M = FunctionMatch{};
  ASSERT_FALSE(bindComparison(Partial, P, S, R));
  EXPECT_FALSE(R.Matches.front().Measured);
  EXPECT_FALSE(R.Matches.front().identical());
}

TEST(BinaryDiff, VocabularyRoundTrips) {
  for (unsigned I = 0; I <= unsigned(FunctionStep::InlinedTailCall); ++I) {
    const auto Step = static_cast<FunctionStep>(I);
    EXPECT_EQ(functionStepNamed(stepName(Step)), Step);
    EXPECT_FALSE(stepDescription(Step).empty());
  }
  for (unsigned I = 0; I <= unsigned(BlockStep::BlockOrder); ++I)
    EXPECT_EQ(blockStepNamed(stepName(static_cast<BlockStep>(I))),
              static_cast<BlockStep>(I));
  for (uint32_t Bit = 0; Bit < 32; ++Bit) {
    const uint32_t Kind = 1u << Bit;
    if (!(AllChanges & Kind))
      continue;
    const auto Names = changeNames(Kind);
    ASSERT_EQ(Names.size(), 1u);
    EXPECT_EQ(changeNamed(Names[0]), static_cast<ChangeKind>(Kind));
  }
  EXPECT_EQ(changeLetters(0).find_first_not_of('.'), std::string::npos);
  for (RowKind K :
       {RowKind::Identical, RowKind::Moved, RowKind::Constant, RowKind::Operand,
        RowKind::Replaced, RowKind::Inserted, RowKind::Deleted})
    EXPECT_EQ(rowKindNamed(rowKindName(K)), K);
}

TEST(BinaryDiff, CancelledDeltaIsNotMeasuredAsUnchanged) {
  const Program P = build(sample(), 0x400000);
  std::atomic<bool> Cancel{true};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  const auto D = compareFunctions(P, 0, P, 0, {}, Control);
  EXPECT_EQ(D.State, ComparisonState::Cancelled);
  EXPECT_FALSE(D.ComputationComplete);
  EXPECT_FALSE(D.AnalysisComplete);
  EXPECT_TRUE(D.Rows.empty());
  ComparisonControl Expired;
  Expired.Deadline = std::chrono::steady_clock::now();
  const auto TimedOut = compareFunctions(P, 0, P, 0, {}, Expired);
  EXPECT_EQ(TimedOut.State, ComparisonState::TimedOut);
  EXPECT_FALSE(TimedOut.ComputationComplete);
  Cancel = false;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t Done, uint64_t) {
    if (Phase == "features" && Done == 1)
      Cancel = true;
  };
  const auto R = compare(P, P, {}, {}, Control);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_EQ(R.StoppedPhase, "features");
  EXPECT_TRUE(R.Matches.empty());
  EXPECT_EQ(R.PrimaryCoverage.Unmatched, P.Functions.size());
}

namespace {
RegionSettings enabledRegions() {
  RegionSettings S;
  S.Enabled = true;
  return S;
}
bool explains(const RegionHypothesis &H, RegionKind Kind) {
  return llvm::is_contained(H.Explanations, Kind);
}
} // namespace

TEST(BinaryDiff, RegionSplitAndMergePreserveManyToManyCoordinates) {
  Fn Whole;
  Whole.Name = "whole";
  Whole.Blocks = {body(10, 6), body(20, 7)};
  Whole.Edges = {{0, 1, EdgeKind::Fallthrough}};
  const auto A = build({Whole}, 0x1000);
  const auto B = build({leaf("part_a", 10, 6), leaf("part_b", 20, 7)}, 0x8000);
  const auto R = findRegions(A, B, enabledRegions(), {});
  ASSERT_EQ(R.State, ComparisonState::Complete);
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  const auto &H = R.Hypotheses[0];
  EXPECT_TRUE(explains(H, RegionKind::Split));
  EXPECT_FALSE(explains(H, RegionKind::Clone));
  EXPECT_EQ(H.PrimaryFunctions.size(), 1u);
  EXPECT_EQ(H.SecondaryFunctions.size(), 2u);
  EXPECT_EQ(H.PrimaryInstructions, 13u);
  EXPECT_EQ(H.PrimaryCoverage, 1);
  EXPECT_TRUE(H.Ambiguous);
  EXPECT_FALSE(validateRegions(R, enabledRegions(), A, B));
  const auto Back = findRegions(B, A, enabledRegions(), {});
  ASSERT_EQ(Back.Hypotheses.size(), 1u);
  EXPECT_TRUE(explains(Back.Hypotheses[0], RegionKind::Merge));
}

TEST(BinaryDiff, RegionInlineAndOutlineUseObservedInternalCalls) {
  Fn Caller;
  Caller.Name = "caller";
  Caller.Blocks = {body(40, 5)};
  Caller.Blocks[0].push_back({"call", "", InstructionFlow::Call, "callee"});
  const auto After = body(50, 5);
  Caller.Blocks[0].insert(Caller.Blocks[0].end(), After.begin(), After.end());
  Fn Callee = leaf("callee", 60, 6);
  Callee.Blocks[0].back().Flow = InstructionFlow::Return;
  Fn Inlined;
  Inlined.Name = "caller";
  Inlined.Blocks = {body(40, 5)};
  const auto Middle = body(60, 6);
  Inlined.Blocks[0].insert(Inlined.Blocks[0].end(), Middle.begin(),
                           Middle.end());
  Inlined.Blocks[0].insert(Inlined.Blocks[0].end(), After.begin(), After.end());
  const auto A = build({Caller, Callee}, 0x1000), B = build({Inlined}, 0x8000);
  const auto R = findRegions(A, B, enabledRegions(), {});
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  EXPECT_TRUE(explains(R.Hypotheses[0], RegionKind::Inline));
  EXPECT_TRUE(explains(R.Hypotheses[0], RegionKind::Merge));
  EXPECT_EQ(R.Hypotheses[0].PrimaryInstructions, 16u);
  const auto Reverse = findRegions(B, A, enabledRegions(), {});
  ASSERT_EQ(Reverse.Hypotheses.size(), 1u);
  EXPECT_TRUE(explains(Reverse.Hypotheses[0], RegionKind::Outline));
}

namespace {
Fn callRegionBody(std::string Name, std::string Target) {
  Fn F;
  F.Name = std::move(Name);
  F.Blocks = {body(411, 3)};
  F.Blocks[0].push_back({"call", "", InstructionFlow::Call, std::move(Target)});
  const auto Suffix = body(527, 3);
  F.Blocks[0].insert(F.Blocks[0].end(), Suffix.begin(), Suffix.end());
  F.Blocks[0].push_back({"ret", "", InstructionFlow::Return});
  return F;
}
Fn callRegionTarget(std::string Name, unsigned Seed) {
  auto F = leaf(std::move(Name), Seed, 1);
  F.Blocks[0].back().Flow = InstructionFlow::Return;
  return F;
}
} // namespace

TEST(BinaryDiff, CallRegionsRetainMovedBodiesAndExactTargetAssumptions) {
  const auto P =
      build({callRegionBody("old", "callee"), callRegionTarget("callee", 77)},
            0x1000);
  auto Public = callRegionBody("public", "callee_new");
  const auto Prefix = body(619, 6);
  Public.Blocks[0].insert(Public.Blocks[0].begin(), Prefix.begin(),
                          Prefix.end());
  const auto S =
      build({Public, callRegionBody("private", "callee_new"),
             callRegionBody("wrong", "other"),
             callRegionTarget("callee_new", 77), callRegionTarget("other", 91)},
            0x8000);
  FunctionMatch M;
  M.Primary = 1;
  M.Secondary = 3;
  auto Settings = enabledRegions();
  const auto Legacy = findRegions(P, S, Settings, {}, {M});
  EXPECT_TRUE(Legacy.Anchors.empty());
  Settings.Calls = true;
  const auto R = findRegions(P, S, Settings, {}, {M});
  ASSERT_EQ(R.State, ComparisonState::Complete);
  ASSERT_EQ(R.Anchors.size(), 2u);
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  EXPECT_TRUE(R.Hypotheses[0].Ambiguous);
  EXPECT_TRUE(explains(R.Hypotheses[0], RegionKind::Clone));
  EXPECT_EQ(R.Hypotheses[0].PrimaryInstructions, 7u);
  EXPECT_EQ(R.Hypotheses[0].SecondaryInstructions, 14u);
  for (const auto &A : R.Anchors) {
    EXPECT_EQ(A.Primary.Function, 0u);
    EXPECT_LT(A.Secondary.Function, 2u);
    EXPECT_EQ(A.Primary.InstructionCount, 7u);
  }
  EXPECT_FALSE(validateRegions(R, Settings, P, S, {}, {M}));
  EXPECT_TRUE(validateRegions(R, Settings, P, S));
  EXPECT_TRUE(findRegions(P, S, Settings, {}).Anchors.empty());
  std::swap(M.Primary, M.Secondary);
  const auto Back = findRegions(S, P, Settings, {}, {M});
  ASSERT_EQ(Back.Anchors.size(), 2u);
  EXPECT_FALSE(validateRegions(Back, Settings, S, P, {}, {M}));
  EXPECT_THROW(findRegions(S, P, Settings, {}, {M, M}), std::invalid_argument);

  for (const auto Flow :
       {InstructionFlow::IndirectCall, InstructionFlow::Unknown}) {
    auto Changed = S;
    Changed.Instructions[Changed.Blocks[0].FirstInstruction + 9].Flow = Flow;
    std::swap(M.Primary, M.Secondary);
    const auto Limited = findRegions(P, Changed, Settings, {}, {M});
    ASSERT_EQ(Limited.Anchors.size(), 1u);
    EXPECT_EQ(Limited.Anchors[0].Secondary.Function, 1u);
    std::swap(M.Primary, M.Secondary);
  }
}

TEST(BinaryDiff, ShortRepeatedCallRegionsNeedDistinctMappedTargets) {
  for (bool Distinct : {false, true}) {
    Fn Before;
    Before.Name = "old";
    auto Shared = body(713, 2);
    Shared.push_back({"call", "", InstructionFlow::Call, "one"});
    const auto Middle = body(719, 2);
    Shared.insert(Shared.end(), Middle.begin(), Middle.end());
    Shared.push_back(
        {"call", "", InstructionFlow::Call, Distinct ? "two" : "one"});
    Shared.push_back(body(727, 1)[0]);
    Before.Blocks = {body(100, 20), Shared};
    auto Public = Before, Private = Before;
    Public.Name = "public";
    Public.Blocks[0] = body(200, 20);
    Private.Name = "private";
    Private.Blocks[0] = body(300, 20);
    const auto P = build(
        {Before, callRegionTarget("one", 77), callRegionTarget("two", 91)},
        0x1000);
    const auto S = build({Public, Private, callRegionTarget("one", 77),
                          callRegionTarget("two", 91)},
                         0x8000);
    FunctionMatch One, Two;
    One.Primary = 1;
    One.Secondary = 2;
    Two.Primary = 2;
    Two.Secondary = 3;
    auto Settings = enabledRegions();
    Settings.Calls = true;
    const auto R = findRegions(P, S, Settings, {}, {One, Two});
    EXPECT_EQ(R.State, ComparisonState::Complete);
    EXPECT_EQ(R.Anchors.size(), Distinct ? 2u : 0u);
    EXPECT_FALSE(validateRegions(R, Settings, P, S, {}, {One, Two}));
    if (Distinct) {
      ASSERT_EQ(R.Hypotheses.size(), 1u);
      EXPECT_TRUE(R.Hypotheses[0].Ambiguous);
      EXPECT_TRUE(explains(R.Hypotheses[0], RegionKind::Clone));
      EXPECT_EQ(R.Hypotheses[0].PrimaryInstructions, 7u);
      EXPECT_EQ(R.Hypotheses[0].SecondaryInstructions, 14u);
      Settings.MaxOccurrences = 1;
      const auto Frequent = findRegions(P, S, Settings, {}, {One, Two});
      EXPECT_TRUE(Frequent.Anchors.empty());
      EXPECT_GT(Frequent.FrequentSeedGroups, 0u);
    } else {
      EXPECT_GT(R.WeakCandidates, 0u);
    }
  }
}

TEST(BinaryDiff, CallRegionsKeepCleanupTailsBesideChangedCalls) {
  for (const auto Flow : {InstructionFlow::Return, InstructionFlow::Jump,
                          InstructionFlow::Branch}) {
    Fn Old;
    Old.Name = "old";
    auto Ops = body(817, 3);
    Ops.push_back({"call", "", InstructionFlow::Call, "before"});
    const auto Suffix = body(823, 2);
    Ops.insert(Ops.end(), Suffix.begin(), Suffix.end());
    Ops.push_back({"call", "", InstructionFlow::Call, "cleanup"});
    Ops.push_back(body(827, 1)[0]);
    Ops.push_back({"transfer", "", Flow});
    Old.Blocks = {body(100, 20), Ops};
    auto New = Old, Copy = Old;
    New.Name = "new";
    New.Blocks[0] = body(200, 20);
    New.Blocks[1][3].Callee = "changed";
    Copy.Name = "copy";
    Copy.Blocks[0] = body(300, 20);
    Copy.Blocks[1][3].Callee = "changed";
    const auto P = build(
        {Old, callRegionTarget("before", 77), callRegionTarget("cleanup", 91)},
        0x1000);
    const auto S = build({New, Copy, callRegionTarget("before", 77),
                          callRegionTarget("changed", 99),
                          callRegionTarget("cleanup", 91)},
                         0x8000);
    FunctionMatch Before, Cleanup;
    Before.Primary = 1;
    Before.Secondary = 2;
    Cleanup.Primary = 2;
    Cleanup.Secondary = 4;
    auto Settings = enabledRegions();
    Settings.Calls = true;
    const auto R = findRegions(P, S, Settings, {}, {Before, Cleanup});
    EXPECT_EQ(R.Anchors.size(), Flow == InstructionFlow::Branch ? 0u : 2u);
    for (const auto &A : R.Anchors) {
      EXPECT_EQ(A.Primary.FirstInstruction, 4u);
      EXPECT_EQ(A.Primary.InstructionCount, 4u);
      EXPECT_EQ(A.Secondary.FirstInstruction, 4u);
    }
    EXPECT_FALSE(validateRegions(R, Settings, P, S, {}, {Before, Cleanup}));
    // The unresolved cleanup call cannot supply terminal support either.
    EXPECT_TRUE(findRegions(P, S, Settings, {}, {Before}).Anchors.empty());
  }
}

TEST(BinaryDiff, CallRegionsBindSavedEvidenceAndMappingEditsTransactionally) {
  const auto P = build(
      {callRegionBody("caller", "callee"), callRegionTarget("callee", 77)},
      0x1000);
  const auto S = build(
      {callRegionBody("caller", "callee"), callRegionTarget("callee", 77)},
      0x8000);
  ComparisonSettings Settings;
  Settings.Regions = enabledRegions();
  Settings.Regions.Calls = true;
  auto R = compare(P, S, Settings);
  ASSERT_EQ(R.Regions.Anchors.size(), 1u);
  const auto Initial = regionAnalysisJSON(R.Regions);
  EXPECT_EQ(Initial.getString("profile"), "native-call-runs-v1");
  EXPECT_EQ(Initial.getString("call_evidence"),
            "current_function_correspondence");
  EXPECT_EQ(readRegionSettings(regionSettingsJSON(Settings.Regions)),
            Settings.Regions);
  EXPECT_THROW(readRegionSettings(llvm::json::Object{{"calls", 1}}),
               std::invalid_argument);
  auto Forged = Initial;
  Forged["call_evidence"] = "semantic_equivalence";
  EXPECT_THROW(readRegionAnalysis(std::move(Forged), Settings.Regions),
               std::invalid_argument);
  auto OldSettings = Settings.Regions;
  OldSettings.Calls = false;
  EXPECT_THROW(readRegionAnalysis(llvm::json::Object(Initial), OldSettings),
               std::invalid_argument);
  EXPECT_FALSE(regionSettingsJSON(OldSettings).get("calls"));

  std::string Error;
  const auto Saved = readComparison(saved(R, P, S), Error);
  ASSERT_TRUE(Saved) << Error;
  ComparisonResult Bound;
  ASSERT_FALSE(bindComparison(*Saved, P, S, Bound));
  EXPECT_EQ(regionAnalysisJSON(Bound.Regions), Initial);
  Override O{OverrideKind::Unmatch, P.Functions[1].Address,
             S.Functions[1].Address};
  std::atomic<bool> Cancel = false;
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phase == "regions:call-context")
      Cancel = true;
  };
  OverrideEffect Effect;
  EXPECT_THROW(applyOverride(R, P, S, O, &Effect, Control),
               ComparisonInterrupted);
  EXPECT_TRUE(R.Overrides.empty());
  EXPECT_EQ(regionAnalysisJSON(R.Regions), Initial);
  ASSERT_FALSE(applyOverride(R, P, S, O, &Effect));
  EXPECT_TRUE(R.Regions.Anchors.empty());
  const auto Edited = readComparison(saved(R, P, S), Error);
  ASSERT_TRUE(Edited) << Error;
  EXPECT_FALSE(bindComparison(*Edited, P, S, Bound));
  auto Stale = *Edited;
  Stale.Regions = Saved->Regions;
  EXPECT_TRUE(bindComparison(Stale, P, S, Bound));
  Cancel = false;
  EXPECT_THROW(revertOverride(R, P, S, Effect, Control), ComparisonInterrupted);
  EXPECT_TRUE(R.Regions.Anchors.empty());
  EXPECT_EQ(R.Overrides.size(), 1u);
  revertOverride(R, P, S, Effect);
  EXPECT_EQ(regionAnalysisJSON(R.Regions), Initial);
  EXPECT_FALSE(
      validateRegions(R.Regions, Settings.Regions, P, S, {}, R.Matches));
  auto Limited = Settings.Regions;
  Limited.MaxWork = 1;
  const auto Stopped = findRegions(P, S, Limited, {}, R.Matches);
  EXPECT_EQ(Stopped.State, ComparisonState::ResourceLimited);
  EXPECT_EQ(Stopped.Work, 1u);
  EXPECT_TRUE(Stopped.Anchors.empty());
  EXPECT_FALSE(validateRegions(Stopped, Limited, P, S, {}, R.Matches));
}

TEST(BinaryDiff, RegionClonesRemainCompetingOverlappingEvidence) {
  const auto A = build({leaf("original", 71, 8)}, 0x1000);
  const auto B =
      build({leaf("clone_a", 71, 8), leaf("clone_b", 71, 8)}, 0x8000);
  const auto R = findRegions(A, B, enabledRegions(), {});
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  const auto &H = R.Hypotheses[0];
  EXPECT_TRUE(explains(H, RegionKind::Clone));
  EXPECT_TRUE(H.Ambiguous);
  EXPECT_EQ(H.PrimaryInstructions, 9u);
  EXPECT_EQ(H.SecondaryInstructions, 18u);
  EXPECT_EQ(H.Anchors.size(), 2u);
  EXPECT_LE(H.Confidence, 0.5);
}

TEST(BinaryDiff, RegionSharedTailsAndModuleMovementAreHypotheses) {
  const auto Tail = [] {
    auto Ops = body(99, 6);
    Ops.push_back({"ret", "", InstructionFlow::Return});
    return Ops;
  }();
  Fn One;
  One.Name = "one";
  One.Module = "old_module";
  One.Blocks = {body(10, 6), Tail};
  Fn Two;
  Two.Name = "two";
  Two.Module = "old_module";
  Two.Blocks = {body(20, 6), Tail};
  Fn Joined;
  Joined.Name = "joined";
  Joined.Module = "new_module";
  Joined.Blocks = {body(10, 6), body(20, 6), Tail};
  const auto R = findRegions(build({One, Two}, 0x1000), build({Joined}, 0x8000),
                             enabledRegions(), {});
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  EXPECT_TRUE(explains(R.Hypotheses[0], RegionKind::TailMerge));
  EXPECT_TRUE(explains(R.Hypotheses[0], RegionKind::ModuleMove));
  EXPECT_EQ(R.Hypotheses[0].PrimaryInstructions, 24u);
  EXPECT_EQ(R.Hypotheses[0].SecondaryInstructions, 18u);
}

TEST(BinaryDiff, RegionFrequentCodeAbstainsAndBudgetsStopDeterministically) {
  std::vector<Fn> Repeated;
  for (unsigned I = 0; I < 20; ++I)
    Repeated.push_back(leaf("copy" + std::to_string(I), 1, 8));
  const auto A = build(Repeated, 0x1000), B = build(Repeated, 0x8000);
  const auto R = findRegions(A, B, enabledRegions(), {});
  EXPECT_EQ(R.State, ComparisonState::Complete);
  EXPECT_TRUE(R.Hypotheses.empty());
  EXPECT_GT(R.FrequentSeedGroups, 0u);
  auto Settings = enabledRegions();
  Settings.MaxSeeds = 2;
  const auto Limited = findRegions(A, B, Settings, {});
  EXPECT_EQ(Limited.State, ComparisonState::ResourceLimited);
  EXPECT_EQ(Limited.PrimarySeeds, 2u);
  EXPECT_TRUE(Limited.Anchors.empty());
  Settings = enabledRegions();
  Settings.MaxWork = 20;
  const auto First = findRegions(A, B, Settings, {}),
             Second = findRegions(A, B, Settings, {});
  EXPECT_EQ(First.State, ComparisonState::ResourceLimited);
  EXPECT_EQ(First.Work, 20u);
  EXPECT_EQ(First.Anchors, Second.Anchors);
}

TEST(BinaryDiff, RegionCancellationDoesNotInventCompletedHypotheses) {
  const auto A = build({leaf("fn", 5, 20)}, 0x1000);
  std::atomic<bool> Cancel{false};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phase == "regions:anchors")
      Cancel = true;
  };
  const auto R = findRegions(A, A, enabledRegions(), Control);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_EQ(R.StoppedPhase, "regions:anchors");
  EXPECT_TRUE(R.Anchors.empty());
  EXPECT_TRUE(R.Hypotheses.empty());
}

TEST(BinaryDiff, RegionRedistributionRetainsMultipleFunctionsOnBothSides) {
  Fn A;
  A.Name = "a";
  A.Blocks = {body(10, 5), body(20, 5)};
  Fn B;
  B.Name = "b";
  B.Blocks = {body(30, 5), body(40, 5)};
  Fn C;
  C.Name = "c";
  C.Blocks = {body(10, 5), body(30, 5)};
  Fn D;
  D.Name = "d";
  D.Blocks = {body(20, 5), body(40, 5)};
  const auto P = build({A, B}, 0x1000), S = build({C, D}, 0x8000);
  const auto R = findRegions(P, S, enabledRegions(), {});
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  const auto &H = R.Hypotheses[0];
  EXPECT_TRUE(explains(H, RegionKind::Redistribution));
  EXPECT_EQ(H.PrimaryFunctions.size(), 2u);
  EXPECT_EQ(H.SecondaryFunctions.size(), 2u);
  EXPECT_EQ(H.Anchors.size(), 4u);
  EXPECT_EQ(H.PrimaryInstructions, 20u);
  EXPECT_EQ(H.SecondaryInstructions, 20u);
  EXPECT_FALSE(validateRegions(R, enabledRegions(), P, S));
}

TEST(BinaryDiff, RegionTailMergingInsideOneFunctionKeepsItsAmbiguity) {
  auto Tail = body(101, 5);
  Tail.push_back({"ret", "", InstructionFlow::Return});
  Fn Before;
  Before.Name = "before";
  Before.Blocks = {Tail, Tail};
  Fn After;
  After.Name = "after";
  After.Blocks = {Tail};
  const auto P = build({Before}, 0x1000), S = build({After}, 0x8000);
  const auto R = findRegions(P, S, enabledRegions(), {});
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  EXPECT_TRUE(explains(R.Hypotheses[0], RegionKind::TailMerge));
  EXPECT_TRUE(R.Hypotheses[0].Ambiguous);
  EXPECT_EQ(R.Hypotheses[0].PrimaryInstructions, 10u);
  EXPECT_EQ(R.Hypotheses[0].SecondaryInstructions, 5u);
  auto Partial = P;
  Partial.Identity.DiscoveryComplete = false;
  const auto Uncertain = findRegions(Partial, S, enabledRegions(), {});
  ASSERT_EQ(Uncertain.Hypotheses.size(), 1u);
  EXPECT_FALSE(Uncertain.Hypotheses[0].AnalysisComplete);
  EXPECT_LT(Uncertain.Hypotheses[0].Confidence, R.Hypotheses[0].Confidence);
}

TEST(BinaryDiff, RegionShortRepeatedFragmentsDoNotJoinUnrelatedLargeBodies) {
  Fn A = leaf("a", 10, 40), B = leaf("b", 20, 40);
  const auto Shared = body(99, 4);
  A.Blocks[0].insert(A.Blocks[0].begin() + 20, Shared.begin(), Shared.end());
  B.Blocks[0].insert(B.Blocks[0].begin() + 20, Shared.begin(), Shared.end());
  const auto P = build({A, B}, 0x1000), S = build({A, B}, 0x8000);
  const auto R = findRegions(P, S, enabledRegions(), {});
  EXPECT_EQ(R.State, ComparisonState::Complete);
  EXPECT_GT(R.WeakCandidates, 0u);
  ASSERT_EQ(R.Hypotheses.size(), 2u);
  for (const auto &H : R.Hypotheses)
    EXPECT_FALSE(H.Ambiguous);
  auto Broad = enabledRegions();
  Broad.MinimumRepeatedInstructions = 4;
  const auto All = findRegions(P, S, Broad, {});
  EXPECT_EQ(All.WeakCandidates, 0u);
  ASSERT_EQ(All.Hypotheses.size(), 1u);
  EXPECT_TRUE(explains(All.Hypotheses[0], RegionKind::Redistribution));
}

TEST(BinaryDiff,
     RegionEvidenceKeepsConstantChangesAndRejectsForgedCoordinates) {
  auto Old = leaf("fn", 90, 20), New = Old;
  New.Blocks[0][8].Operands = "r3, 7";
  const auto A = build({Old}, 0x1000), B = build({New}, 0x8000);
  ComparisonSettings Settings;
  Settings.Regions = enabledRegions();
  const auto R = compare(A, B, Settings);
  ASSERT_EQ(R.Matches.size(), 1u);
  EXPECT_FALSE(R.Matches[0].identical());
  ASSERT_EQ(R.Regions.Hypotheses.size(), 1u);
  EXPECT_LT(R.Regions.Hypotheses[0].PrimaryCoverage, 1);
  for (const auto &Anchor : R.Regions.Anchors) {
    const auto &Span = Anchor.Primary;
    EXPECT_TRUE(Span.FirstInstruction > 8 ||
                Span.FirstInstruction + Span.InstructionCount <= 8);
  }
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  writeComparison(OS, R, A, B);
  std::string Error;
  const auto Saved = readComparison(Text, Error);
  ASSERT_TRUE(Saved) << Error;
  ComparisonResult Restored;
  EXPECT_FALSE(bindComparison(*Saved, A, B, Restored));
  EXPECT_EQ(Restored.Regions.Anchors, R.Regions.Anchors);
  auto Forged = *Saved;
  Forged.Regions.Anchors[0].Primary.FirstInstruction = 8;
  EXPECT_TRUE(bindComparison(Forged, A, B, Restored));
  Forged = *Saved;
  Forged.Regions.Hypotheses[0].Confidence = 1;
  EXPECT_TRUE(bindComparison(Forged, A, B, Restored));
  EXPECT_EQ(Restored.Regions.Anchors, R.Regions.Anchors);
}

TEST(BinaryDiff, RegionClaimsRequireUniqueSeedsAndConnectedMembership) {
  const auto Duplicated =
      build({leaf("copy_a", 4, 12), leaf("copy_b", 4, 12)}, 0x1000);
  const auto Copy = build({leaf("copy", 4, 12)}, 0x8000);
  const auto Settings = enabledRegions();
  auto Clones = findRegions(Duplicated, Copy, Settings, {});
  ASSERT_EQ(Clones.Hypotheses.size(), 1u);
  for (auto &Anchor : Clones.Anchors) {
    ASSERT_FALSE(Anchor.UniqueSeed);
    Anchor.UniqueSeed = true;
  }
  // Forge both the source flag and the score derived from it.
  Clones.Hypotheses[0].Confidence = 0.95;
  const auto UniqueError = validateRegions(Clones, Settings, Duplicated, Copy);
  ASSERT_TRUE(UniqueError);
  EXPECT_NE(UniqueError->find("unique seed"), std::string::npos);

  const auto P = build({leaf("first", 10, 12), leaf("second", 50, 12)}, 0x1000);
  const auto R = findRegions(P, P, Settings, {});
  ASSERT_EQ(R.Hypotheses.size(), 2u);
  auto Forged = R;
  Forged.Hypotheses[0].Anchors.insert(Forged.Hypotheses[0].Anchors.end(),
                                      R.Hypotheses[1].Anchors.begin(),
                                      R.Hypotheses[1].Anchors.end());
  llvm::sort(Forged.Hypotheses[0].Anchors);
  Forged.Hypotheses.pop_back();
  const auto ConnectedError = validateRegions(Forged, Settings, P, P);
  ASSERT_TRUE(ConnectedError);
  EXPECT_NE(ConnectedError->find("connected component"), std::string::npos);
  Forged = R;
  Forged.Hypotheses.pop_back();
  EXPECT_TRUE(validateRegions(Forged, Settings, P, P));
}

TEST(BinaryDiff, EditsStopTransactionallyBeforePublishingResultsOrEffects) {
  const auto P = build(sample(), 0x400000);
  const auto S = build(sample(), 0x800000);
  auto R = compare(P, S, {});
  const auto Original = saved(R, P, S);
  const Override Decision{OverrideKind::Match, named(P, "leaf1").Address,
                          named(S, "leaf2").Address};
  OverrideEffect Effect;
  ASSERT_FALSE(applyOverride(R, P, S, Decision, &Effect));
  ASSERT_FALSE(Effect.Remeasured.empty());
  const auto Edited = R;
  revertOverride(R, P, S, Effect);
  ASSERT_EQ(saved(R, P, S), Original);
  std::atomic<bool> Cancel = false;
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  const auto Caller = std::this_thread::get_id();
  for (const auto &[Stage, Minimum] :
       std::vector<std::pair<std::string, uint64_t>>{{"prepare", 0},
                                                     {"mapping", 0},
                                                     {"measure", 0},
                                                     {"measure", 1},
                                                     {"publish", 0}}) {
    SCOPED_TRACE(Stage + ":" + std::to_string(Minimum));
    Cancel = false;
    bool Seen = false;
    Control.Progress = [&](llvm::StringRef Phase, uint64_t Done, uint64_t) {
      EXPECT_EQ(std::this_thread::get_id(), Caller);
      if (Phase == Stage && Done >= Minimum) {
        Seen = true;
        Cancel = true;
      }
    };
    EXPECT_THROW(applyOverride(R, P, S, Decision, &Effect, Control),
                 ComparisonInterrupted);
    EXPECT_TRUE(Seen);
    EXPECT_EQ(saved(R, P, S), Original);
    // The caller's existing undo effect must still restore its own edit.
    auto CheckEffect = Edited;
    revertOverride(CheckEffect, P, S, Effect);
    EXPECT_EQ(saved(CheckEffect, P, S), Original);
  }
  Cancel = false;
  Control.Progress = {};
  Control.Deadline = std::chrono::steady_clock::now();
  try {
    (void)applyOverride(R, P, S, Decision, &Effect, Control);
    FAIL() << "Expired edit deadline was ignored";
  } catch (const ComparisonInterrupted &Stopped) {
    EXPECT_EQ(Stopped.State, ComparisonState::TimedOut);
  }
  EXPECT_EQ(saved(R, P, S), Original);
  Control.Deadline.reset();
  ASSERT_FALSE(applyOverride(R, P, S, Decision, &Effect, Control));
  EXPECT_EQ(saved(R, P, S), saved(Edited, P, S));
  for (const auto *Stage : {"prepare", "mapping", "publish"}) {
    SCOPED_TRACE(Stage);
    Cancel = false;
    bool Seen = false;
    Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
      if (Phase == Stage) {
        Seen = true;
        Cancel = true;
      }
    };
    EXPECT_THROW(revertOverride(R, P, S, Effect, Control),
                 ComparisonInterrupted);
    EXPECT_TRUE(Seen);
    EXPECT_EQ(saved(R, P, S), saved(Edited, P, S));
  }
  Cancel = false;
  Control.Progress = {};
  revertOverride(R, P, S, Effect, Control);
  EXPECT_EQ(saved(R, P, S), Original);
}

TEST(BinaryDiff,
     CancelledPairingKeepsAmbiguitiesAndFailedCallbacksKeepHistory) {
  const auto P = build({leaf("first", 7, 8), leaf("second", 7, 8)}, 0x1000);
  const auto S = build({leaf("first", 7, 8), leaf("second", 7, 8)}, 0x8000);
  ComparisonSettings Settings;
  Settings.UseNames = false;
  auto R = compare(P, S, Settings);
  ASSERT_FALSE(R.Ambiguities.empty());
  const auto Before = saved(R, P, S);
  OverrideEffect Effect;
  ComparisonControl Control;
  Control.Progress = [](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phase == "publish")
      throw std::runtime_error("host callback failed");
  };
  EXPECT_THROW(applyOverride(R, P, S,
                             {OverrideKind::Match, named(P, "first").Address,
                              named(S, "second").Address},
                             &Effect, Control),
               std::runtime_error);
  EXPECT_EQ(saved(R, P, S), Before);
  EXPECT_FALSE(Effect.Added);
  EXPECT_TRUE(Effect.Ambiguities.empty());
}

TEST(BinaryDiff, SavedRestoreStopsTransactionallyAndKeepsHistoricalStates) {
  const Program P = build(sample(), 0x400000, {}, std::string(64, 'b'));
  const Program S = build(sample(), 0x800000, {}, std::string(64, 'c'));
  const auto Original = compare(P, S, {});
  const auto Text = saved(Original, P, S);
  std::string Error;
  const auto Read = readComparison(Text, Error);
  ASSERT_TRUE(Read) << Error;
  auto Retained = Original;
  ASSERT_FALSE(applyOverride(Retained, P, S,
                             {OverrideKind::Confirm, named(P, "leaf0").Address,
                              named(S, "leaf0").Address}));
  const auto Before = saved(Retained, P, S);
  std::atomic<bool> Cancel = true;
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  EXPECT_THROW(readComparison(Text, Error, Control), ComparisonInterrupted);
  EXPECT_THROW(bindComparison(*Read, P, S, Retained, nullptr, Control),
               ComparisonInterrupted);
  EXPECT_EQ(saved(Retained, P, S), Before);
  Cancel = false;
  Control.Deadline = std::chrono::steady_clock::now();
  try {
    (void)bindComparison(*Read, P, S, Retained, nullptr, Control);
    FAIL() << "Expired restore deadline was ignored";
  } catch (const ComparisonInterrupted &Stopped) {
    EXPECT_EQ(Stopped.State, ComparisonState::TimedOut);
  }
  EXPECT_EQ(saved(Retained, P, S), Before);
  Control.Deadline.reset();
  const auto Caller = std::this_thread::get_id();
  for (const auto *Stage :
       {"parse", "records", "identity", "decisions", "dataflow", "reviews",
        "regions", "flow_regions", "measure", "publish"}) {
    SCOPED_TRACE(Stage);
    Cancel = false;
    bool Seen = false;
    Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
      EXPECT_EQ(std::this_thread::get_id(), Caller);
      if (Phase == Stage) {
        Seen = true;
        Cancel = true;
      }
    };
    EXPECT_THROW(
        {
          const auto Next = readComparison(Text, Error, Control);
          ASSERT_TRUE(Next) << Error;
          (void)bindComparison(*Next, P, S, Retained, nullptr, Control);
        },
        ComparisonInterrupted);
    EXPECT_TRUE(Seen);
    EXPECT_EQ(saved(Retained, P, S), Before);
  }
  Cancel = false;
  Control.Progress = [&](llvm::StringRef, uint64_t, uint64_t) {
    EXPECT_EQ(std::this_thread::get_id(), Caller);
  };
  // Exercise measurement workers without invoking a user callback there.
  neverd::setWorkerThreadCount(4);
  const auto ResetThreads =
      llvm::scope_exit([] { neverd::setWorkerThreadCount(0); });
  for (const auto State :
       {ComparisonState::Complete, ComparisonState::Cancelled,
        ComparisonState::TimedOut, ComparisonState::ResourceLimited}) {
    auto Historical = *Read;
    Historical.State = State;
    ASSERT_FALSE(bindComparison(Historical, P, S, Retained, nullptr, Control));
    EXPECT_EQ(Retained.State, State);
    EXPECT_EQ(Retained.Matches.size(), Original.Matches.size());
  }
}

TEST(BinaryDiff, RestoreStopAfterParsingConsumesSuccessAndFailure) {
  const Program P = build(sample(), 0x400000, {}, std::string(64, 'b'));
  const auto Text = saved(compare(P, P, {}), P, P);
  for (const std::string Input : {Text, std::string("{invalid")}) {
    std::atomic<bool> Cancel = false;
    bool Parsed = false;
    ComparisonControl Control;
    Control.Cancel = &Cancel;
    Control.Progress = [&](llvm::StringRef Phase, uint64_t Done,
                           uint64_t Total) {
      if (Phase == "parse" && Done == Total && Done) {
        Parsed = true;
        Cancel = true;
      }
    };
    std::string Error;
    EXPECT_THROW(readComparison(Input, Error, Control), ComparisonInterrupted);
    EXPECT_TRUE(Parsed);
  }
}

TEST(BinaryDiff, RestoreDeadlineDuringParallelMeasurementKeepsThePriorResult) {
  std::vector<Fn> Functions;
  for (unsigned I = 0; I < 8; ++I)
    Functions.push_back(leaf("wide" + std::to_string(I), I * 3, 60000));
  const auto P = build(Functions, 0x400000, {}, std::string(64, 'b'));
  neverd::setWorkerThreadCount(4);
  const auto ResetThreads =
      llvm::scope_exit([] { neverd::setWorkerThreadCount(0); });
  const auto Original = compare(P, P, {});
  ASSERT_EQ(Original.State, ComparisonState::Complete);
  const auto Text = saved(Original, P, P);
  std::string Error;
  const auto Saved = readComparison(Text, Error);
  ASSERT_TRUE(Saved) << Error;
  auto Retained = Original;
  bool Started = false, Finished = false;
  const auto Caller = std::this_thread::get_id();
  ComparisonControl Control;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t Done, uint64_t Total) {
    EXPECT_EQ(std::this_thread::get_id(), Caller);
    if (Phase != "measure")
      return;
    if (Done == 0) {
      Started = true;
      // Expire after the coordinator enters measurement; a returned stopped
      // delta must propagate as a typed stop, never as forged saved evidence.
      Control.Deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    } else if (Done == Total) {
      Finished = true;
    }
  };
  try {
    const auto Invalid =
        bindComparison(*Saved, P, P, Retained, nullptr, Control);
    FAIL() << "Expected a typed measurement stop, got "
           << Invalid.value_or("success");
  } catch (const ComparisonInterrupted &Stopped) {
    EXPECT_EQ(Stopped.State, ComparisonState::TimedOut);
  }
  EXPECT_TRUE(Started);
  EXPECT_FALSE(Finished);
  EXPECT_EQ(saved(Retained, P, P), Text);
  EXPECT_EQ(compareFunctions(P, 0, P, 0, {}).State, ComparisonState::Complete);
}
