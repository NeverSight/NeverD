//===- ResolverPendingWalksTests.cpp - Provisional proof lifetimes --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks active dependency lifetimes against an independent set model.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "jumptable/ResolverPendingWalks.h"

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

using neverd::detail::ResolverPendingWalks;

TEST(ResolverPendingWalks, SparseDependenciesSurviveUnrelatedReturns) {
  ResolverPendingWalks Walks;
  ResolverPendingWalks::Scope Outer(Walks);
  ResolverPendingWalks::Pending Saved;
  {
    ResolverPendingWalks::Scope Unrelated(Walks);
    {
      ResolverPendingWalks::Scope Inner(Walks);
      Walks.observe(Outer.Self);
      Walks.observe(Inner.Self);
      Saved = Inner.finish();
    }
    EXPECT_TRUE(Walks.live(Saved));
    Unrelated.finish();
  }
  EXPECT_TRUE(Walks.live(Saved));
  EXPECT_TRUE(Outer.finish().empty());
  EXPECT_FALSE(Walks.live(Saved));
  // A new activation at the same depth must not revive a retired dependency.
  ResolverPendingWalks::Scope Replacement(Walks);
  EXPECT_FALSE(Walks.live(Saved));
}

TEST(ResolverPendingWalks, CompletedMemoHitsPropagateTheirDependencies) {
  ResolverPendingWalks Walks;
  ResolverPendingWalks::Scope Outer(Walks);
  ResolverPendingWalks::Pending Saved;
  {
    ResolverPendingWalks::Scope Child(Walks);
    Walks.observe(Outer.Self);
    Saved = Child.finish();
  }
  {
    ResolverPendingWalks::Scope Sibling(Walks);
    Walks.observe(Saved);
    auto Hit = Sibling.finish();
    EXPECT_TRUE(Walks.live(Hit));
    EXPECT_TRUE(Outer.finish().empty());
    EXPECT_FALSE(Walks.live(Hit));
  }
}

TEST(ResolverPendingWalks, ActualDependenciesMatchIndependentActivationSets) {
  // The oracle knows only unique invocation IDs and explicit set unions. It
  // does not use stack intervals or the production bit representation.
  for (unsigned Depth :
       {1u, 4u, 63u, 64u, 65u, 127u, 128u, 129u, 255u, 256u, 257u, 300u}) {
    SCOPED_TRACE(Depth);
    ResolverPendingWalks Walks;
    uint64_t Next = 0;
    struct Frame {
      uint64_t ID;
      std::set<uint64_t> Dependencies;
      std::unique_ptr<ResolverPendingWalks::Scope> Scope;
    };
    struct Saved {
      std::set<uint64_t> Dependencies;
      ResolverPendingWalks::Pending Pending;
    };
    std::vector<Frame> Frames;
    std::vector<Saved> Snapshots;
    std::set<uint64_t> Active;
    auto Check = [&] {
      for (const auto &S : Snapshots) {
        const bool Live =
            std::includes(Active.begin(), Active.end(), S.Dependencies.begin(),
                          S.Dependencies.end());
        if (Depth <= 256)
          EXPECT_EQ(Walks.live(S.Pending), Live);
        else if (!Live)
          EXPECT_FALSE(Walks.live(S.Pending));
      }
    };
    // Repeat at identical depths so snapshots also face activation reuse.
    for (unsigned Round = 0; Round < 3; ++Round) {
      for (unsigned I = 0; I < Depth; ++I) {
        const uint64_t ID = ++Next;
        Active.insert(ID);
        Frames.push_back(
            {ID, {}, std::make_unique<ResolverPendingWalks::Scope>(Walks)});
        for (unsigned J = 0; J <= I; ++J) {
          if ((J * 17 + I * 11 + Round * 13) % 23 != 0 && J != I)
            continue;
          Walks.observe(Frames[J].Scope->Self);
          Frames.back().Dependencies.insert(Frames[J].ID);
        }
      }
      while (!Frames.empty()) {
        auto &F = Frames.back();
        const auto Pending = F.Scope->finish();
        F.Dependencies.erase(F.ID);
        Active.erase(F.ID);
        const auto Dependencies = std::move(F.Dependencies);
        Frames.pop_back();
        if (!Frames.empty())
          Frames.back().Dependencies.insert(Dependencies.begin(),
                                            Dependencies.end());
        Snapshots.push_back({Dependencies, Pending});
        Check();
      }
      Check();
    }
  }
}
