//===- X86RegistrationRegions.cpp - x86 scopes as C regions ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86RegistrationRegions.h"

#include "../HighCFSimplifyDetail.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <functional>

namespace neverd {
namespace {

using Stretch = RegistrationDisplayFacts::Stretch;

/// Where a statement prints: the function's own statements, or the body or the
/// clause of a scope's `__try`.
using Place = int32_t;
constexpr Place kFunctionPlace = 0;

Place bodyOf(size_t Scope) { return static_cast<Place>(Scope) * 2 + 1; }
Place clauseOf(size_t Scope) { return static_cast<Place>(Scope) * 2 + 2; }
size_t scopeOf(Place P) { return static_cast<size_t>(P - 1) / 2; }
bool isClause(Place P) { return P != kFunctionPlace && P % 2 == 0; }

bool hasLabel(const HighStmt &S) { return S.Addr && S.Addr != InvalidVA; }

/// The address a jump names to run the loop \p S from its head.
std::optional<va_t> loopHead(const HighStmt &S) {
  if (S.Kind == StmtKind::While && S.LoopHeaderAddr)
    return S.LoopHeaderAddr;
  return std::nullopt;
}

bool isEmptyAnchor(const HighStmt &S) {
  return S.Kind == StmtKind::Nop ||
         (S.Kind == StmtKind::Block && S.Body.empty());
}

HighStmt jumpTo(va_t Target) {
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = Target;
  return Jump;
}

/// The address a jump names to run \p S: a try's is its first statement's.
std::optional<va_t> firstLabel(const HighStmt &S) {
  if (hasLabel(S))
    return S.Addr;
  if (S.Kind != StmtKind::SEHTry)
    return std::nullopt;
  for (const HighStmt &Inner : S.Body) {
    if (const auto Label = firstLabel(Inner))
      return Label;
    if (!isEmptyAnchor(Inner))
      break;
  }
  return std::nullopt;
}

/// How the scopes nest as C prints them: by their enclosing levels, but for a
/// scope entered in a handler, which is in that handler's clause.
class PlaceTree {
public:
  explicit PlaceTree(const RegistrationRegionFacts &In)
      : In(In), Scopes(In.Scopes), HandlerOf(In.Scopes.size()) {
    for (const Stretch &S : In.Facts.Stretches)
      noteScopesInHandler(S);
    Parents.assign(Scopes.size() * 2 + 1, kFunctionPlace);
    for (size_t I = 0; I < Scopes.size(); ++I) {
      const int32_t Outer = Scopes[I].EnclosingLevel;
      Place Holder = isScope(Outer) ? bodyOf(Outer) : kFunctionPlace;
      if (HandlerOf[I] && !(isScope(Outer) && HandlerOf[Outer] == HandlerOf[I]))
        Holder = clauseOf(*HandlerOf[I]);
      Parents[bodyOf(I)] = Parents[clauseOf(I)] = Holder;
    }
    // A table no compiler writes could loop, and has no place to print from.
    for (size_t I = 1; I < Parents.size(); ++I)
      if (!reachesFunction(static_cast<Place>(I)))
        Parents[I] = kFunctionPlace;
  }

  bool isBlock(size_t Scope) const {
    return In.Blocks.count(Scopes[Scope].HandlerVA) != 0;
  }

  /// A block's clause has no statement of its own: its code stays beside it.
  Place normal(Place P) const {
    while (isClause(P) && isBlock(scopeOf(P)))
      P = Parents[P];
    return P;
  }

  Place parent(Place P) const { return normal(Parents[P]); }

  bool within(Place P, Place Around) const {
    for (P = normal(P); P != Around; P = parent(P))
      if (P == kFunctionPlace)
        return false;
    return true;
  }

  Place common(Place A, Place B) const {
    for (A = normal(A); !within(B, A);)
      A = parent(A);
    return A;
  }

  Place ofLevel(int32_t Level) const {
    return isScope(Level) ? bodyOf(Level) : kFunctionPlace;
  }

  Place ofStretch(const Stretch &S) const {
    int32_t Level = S.Levels.front();
    for (int32_t Other : S.Levels)
      Level = commonLevel(Level, Other);
    if (S.Owner)
      if (const auto Root = In.ScopeOfRoot.find(S.Owner);
          Root != In.ScopeOfRoot.end()) {
        const size_t Handler = Root->second;
        if (sameLevel(Level, Scopes[Handler].EnclosingLevel))
          return normal(clauseOf(Handler));
        if (isScope(Level) && HandlerOf[Level] == Handler)
          return bodyOf(Level);
        // A local unwind left the handler's scopes.
      }
    return ofLevel(Level);
  }

private:
  const RegistrationRegionFacts &In;
  const std::vector<RegistrationScopeRecord> &Scopes;
  std::vector<std::optional<size_t>> HandlerOf;
  std::vector<Place> Parents;

  bool isScope(int32_t Level) const {
    return Level >= 0 && static_cast<size_t>(Level) < Scopes.size();
  }

  bool sameLevel(int32_t A, int32_t B) const {
    return A == B || (!isScope(A) && !isScope(B));
  }

  int32_t commonLevel(int32_t A, int32_t B) const {
    for (size_t StepA = 0; StepA <= Scopes.size() && isScope(A); ++StepA) {
      int32_t Up = B;
      for (size_t StepB = 0; StepB <= Scopes.size() && isScope(Up); ++StepB) {
        if (Up == A)
          return A;
        Up = Scopes[Up].EnclosingLevel;
      }
      A = Scopes[A].EnclosingLevel;
    }
    return -1;
  }

  /// The levels code of a handler runs at, past the one around its scope, are
  /// the scopes it enters.
  void noteScopesInHandler(const Stretch &S) {
    if (!S.Owner)
      return;
    const auto Root = In.ScopeOfRoot.find(S.Owner);
    if (Root == In.ScopeOfRoot.end())
      return;
    const size_t Handler = Root->second;
    const int32_t Around = Scopes[Handler].EnclosingLevel;
    for (int32_t Level : S.Levels) {
      std::vector<size_t> Entered;
      int32_t At = Level;
      for (size_t Step = 0; Step <= Scopes.size() && isScope(At) &&
                            !sameLevel(At, Around) && Entered.size() <= Step;
           ++Step) {
        Entered.push_back(static_cast<size_t>(At));
        At = Scopes[At].EnclosingLevel;
      }
      if (!sameLevel(At, Around))
        continue;
      for (size_t Scope : Entered)
        if (Scope != Handler && !HandlerOf[Scope])
          HandlerOf[Scope] = Handler;
    }
  }

  bool reachesFunction(Place P) const {
    for (size_t Step = 0; Step < Parents.size(); ++Step) {
      if (P == kFunctionPlace)
        return true;
      P = Parents[P];
    }
    return false;
  }
};

/// Builds the regions: each scope's statement sits in the list its first
/// statement was found in, and every other statement of the scope moves to it.
class RegionPlacer {
public:
  explicit RegionPlacer(const RegistrationRegionFacts &In)
      : In(In), Tree(In), Builds(In.Scopes.size()), Home(In.Scopes.size()) {}

  std::vector<HighStmt> run(std::vector<HighStmt> Stmts) {
    Targets = gotoTargets(Stmts);
    // A phi's copy carries the address of the jump it runs before, and
    // structuring can leave it in an arm with that jump after the `if`.  No
    // jump lands on it, and it names nothing: it runs right after the statement
    // before it.
    walkStmts(Stmts, [&](HighStmt &S) {
      if (S.IsPhiCopy && !Targets.count(S.Addr))
        S.Addr = 0;
    });
    std::vector<HighStmt> Function;
    placeList(std::move(Stmts), kFunctionPlace, std::nullopt, false, Function);
    // What moves can hold more that does.
    for (size_t I = 0; I < Moved.size(); ++I) {
      MovedCode Code = std::move(Moved[I]);
      std::vector<HighStmt> &Into =
          destination(Code.At, kFunctionPlace, /*Self=*/0, Function);
      for (size_t K = 0; K < Code.Stmts.size(); ++K) {
        placeChildren(Code.Stmts[K], Code.At,
                      nextLabel(Code.Stmts, K + 1, std::nullopt));
        Into.push_back(std::move(Code.Stmts[K]));
      }
    }
    assemble(Function);
    return Function;
  }

private:
  struct Region {
    std::vector<HighStmt> Body;
    std::vector<HighStmt> Clause;
  };
  struct Group {
    size_t Begin;
    size_t End;
    std::optional<Place> Real;
    Place At;
    /// Whether its own statements do nothing a guard could tell.
    bool DoesNothing;
    bool Ends;
    std::optional<va_t> Label;
  };
  struct MovedCode {
    Place At;
    std::vector<HighStmt> Stmts;
  };

  const RegistrationRegionFacts &In;
  PlaceTree Tree;
  std::vector<Region> Builds;
  /// The list each scope's statement went in.
  std::vector<std::optional<unsigned>> Home;
  std::vector<MovedCode> Moved;
  unsigned NextList = 0;
  std::set<va_t> Targets;

  const Stretch *stretchAt(va_t Address) const {
    const auto &All = In.Facts.Stretches;
    auto After = std::upper_bound(
        All.begin(), All.end(), Address,
        [](va_t A, const Stretch &S) { return A < S.Range.Begin; });
    if (After == All.begin())
      return nullptr;
    --After;
    return After->Range.contains(Address) ? &*After : nullptr;
  }

  /// Where \p S belongs by what it does, or nullopt when a guard cannot tell.
  std::optional<Place> realPlace(const HighStmt &S) const {
    switch (S.Kind) {
    case StmtKind::Nop:
    case StmtKind::Goto:
    case StmtKind::Break:
    case StmtKind::Continue:
      return std::nullopt;
    default:
      break;
    }
    for (const va_t Head : {S.Addr, S.LoopHeaderAddr})
      if (Head && Head != InvalidVA)
        if (const Stretch *At = stretchAt(Head); At && !At->Quiet)
          return Tree.ofStretch(*At);
    std::optional<Place> Common;
    auto Note = [&](const std::vector<HighStmt> &List) {
      for (const HighStmt &Child : List)
        if (const auto Real = realPlace(Child))
          Common = Common ? Tree.common(*Common, *Real) : *Real;
    };
    Note(S.Body);
    Note(S.ElseBody);
    for (const SwitchCase &Case : S.Cases)
      Note(Case.Body);
    Note(S.DefaultBody);
    // A branch on the frame or a register runs at its own level, whatever the
    // code it chooses between does.
    if (Common)
      for (const va_t Head : {S.Addr, S.LoopHeaderAddr})
        if (Head && Head != InvalidVA)
          if (const Stretch *At = stretchAt(Head))
            return Tree.ofStretch(*At);
    return Common;
  }

  /// Where a statement a guard cannot tell about was, by its level.
  Place levelPlace(const HighStmt &S, Place Around) const {
    if (hasLabel(S))
      if (const Stretch *At = stretchAt(S.Addr))
        return Tree.ofStretch(*At);
    return Around;
  }

  static bool doesNothing(const HighStmt &S) {
    switch (S.Kind) {
    case StmtKind::Nop:
    case StmtKind::Goto:
    case StmtKind::Break:
    case StmtKind::Continue:
      return true;
    case StmtKind::Block:
      return S.Body.empty();
    case StmtKind::Return:
      return !S.RetVal || S.RetVal->Kind == ExprKind::Const ||
             S.RetVal->Kind == ExprKind::Undef;
    default:
      return false;
    }
  }

  /// A statement that leaves a loop or a switch has to stay in it.
  static bool bindsToALoop(const HighStmt &S) {
    bool Binds = false;
    walkStmts(std::vector<HighStmt>{S}, [&](const HighStmt &Inner) {
      Binds |=
          Inner.Kind == StmtKind::Break || Inner.Kind == StmtKind::Continue;
    });
    return Binds;
  }

  /// What a jump names to run \p List from \p From on.
  static std::optional<va_t> nextLabel(const std::vector<HighStmt> &List,
                                       size_t From, std::optional<va_t> After) {
    for (size_t I = From; I < List.size(); ++I) {
      const HighStmt &S = List[I];
      if (hasLabel(S))
        return S.Addr;
      if (const auto Head = loopHead(S))
        return Head;
      if (S.Kind == StmtKind::Goto)
        return S.GotoTarget;
      if (!isEmptyAnchor(S))
        return std::nullopt;
    }
    return After;
  }

  std::vector<Group> groupsOf(const std::vector<HighStmt> &Stmts,
                              Place Around) const {
    std::vector<Group> Groups;
    for (size_t I = 0; I < Stmts.size();) {
      // A statement with no address runs right after the one before it, and a
      // jump to an address lands on the first statement with it.
      size_t End = I + 1;
      while (End < Stmts.size() &&
             ((!hasLabel(Stmts[End]) && !loopHead(Stmts[End])) ||
              Stmts[End].Addr == Stmts[I].Addr))
        ++End;
      Group G;
      G.Begin = I;
      G.End = End;
      G.Real = realPlace(Stmts[I]);
      G.At = G.Real ? *G.Real : levelPlace(Stmts[I], Around);
      G.DoesNothing = true;
      G.Ends = false;
      for (size_t K = I; K < End; ++K) {
        G.DoesNothing &= doesNothing(Stmts[K]);
        G.Ends |= highStmtEndsItsBlock(Stmts[K]) || In.NeverComesBack(Stmts[K]);
      }
      if (hasLabel(Stmts[I]))
        G.Label = Stmts[I].Addr;
      else if (const auto Head = loopHead(Stmts[I]))
        G.Label = *Head;
      Groups.push_back(G);
      I = End;
    }
    // Code a guard cannot tell about, between statements of one scope with no
    // way past it, keeps the scope whole.
    for (size_t I = 0; I < Groups.size(); ++I) {
      if (Groups[I].Real)
        continue;
      size_t Last = I;
      bool Falls = I > 0 && !Groups[I - 1].Ends;
      while (Last + 1 < Groups.size() && !Groups[Last + 1].Real) {
        Falls &= !Groups[Last].Ends;
        ++Last;
      }
      Falls &= !Groups[Last].Ends;
      if (Falls && Last + 1 < Groups.size()) {
        const Place Whole = Tree.common(Groups[I - 1].At, Groups[Last + 1].At);
        for (size_t K = I; K <= Last; ++K)
          if (Groups[K].At != Whole && Tree.within(Whole, Groups[K].At))
            Groups[K].At = Whole;
      }
      I = Last;
    }
    return Groups;
  }

  /// Whether the group at \p I is the first in its list of the code only a
  /// runtime entry runs.
  bool startsRuntimeEntry(const std::vector<HighStmt> &Stmts,
                          const std::vector<Group> &Groups, size_t I) const {
    const auto OwnerOf = [&](size_t K) -> va_t {
      if (!Groups[K].Label)
        return 0;
      const Stretch *At = stretchAt(*Groups[K].Label);
      return At ? At->Owner : 0;
    };
    const va_t Owner = OwnerOf(I);
    return Owner && (I == 0 || OwnerOf(I - 1) != Owner);
  }

  bool belongsElsewhere(Place P, Place Around, unsigned Self) const {
    for (P = Tree.normal(P); P != Around; P = Tree.parent(P)) {
      if (P == kFunctionPlace)
        return true;
      const auto &Holder = Home[scopeOf(P)];
      if (Holder && *Holder != Self)
        return true;
    }
    return false;
  }

  HighStmt makeTry(size_t Scope) const {
    const RegistrationScopeRecord &Record = In.Scopes[Scope];
    HighEHClause Clause;
    Clause.Kind = Record.IsFinally ? HighEHClauseKind::SEHFinally
                                   : HighEHClauseKind::SEHExcept;
    Clause.FilterOrActionVA =
        Record.IsFinally ? Record.HandlerVA : Record.FilterVA;
    Clause.HandlerVA = Record.HandlerVA;
    Clause.State = static_cast<int32_t>(Scope);
    Clause.ParseStatus = In.ParseStatus;
    // An arm holds its handler however little that does; a clause with no
    // statement would otherwise name its handler in the listing.
    if (!Record.IsFinally)
      Clause.FinallyKind = HighEHClause::FinallyBody::WholeBody;
    if (const auto Value = In.FilterValues.find(Record.FilterVA);
        !Record.IsFinally && Value != In.FilterValues.end())
      Clause.FilterValue = Value->second;
    HighStmt Try;
    Try.Kind = StmtKind::SEHTry;
    Try.EHClauses.push_back(std::move(Clause));
    Try.EHClauseBodies.resize(1);
    Try.EHIsReducible = !Tree.isBlock(Scope);
    return Try;
  }

  /// The list \p P's statements go in, seen from the list \p Out at \p Around.
  std::vector<HighStmt> &destination(Place P, Place Around, unsigned Self,
                                     std::vector<HighStmt> &Out) {
    P = Tree.normal(P);
    if (P == Around)
      return Out;
    const size_t Scope = scopeOf(P);
    if (!Home[Scope]) {
      std::vector<HighStmt> &Holder =
          destination(Tree.parent(P), Around, Self, Out);
      Holder.push_back(makeTry(Scope));
      Home[Scope] = Self;
    }
    return isClause(P) ? Builds[Scope].Clause : Builds[Scope].Body;
  }

  void placeChildren(HighStmt &S, Place At, std::optional<va_t> Next) {
    auto Into = [&](std::vector<HighStmt> &Child, std::optional<va_t> After,
                    bool InLoop) {
      if (Child.empty())
        return;
      std::vector<HighStmt> Stmts = std::move(Child);
      Child.clear();
      placeList(std::move(Stmts), At, After, InLoop, Child);
    };
    const bool Loop = S.Kind == StmtKind::While || S.Kind == StmtKind::For ||
                      S.Kind == StmtKind::DoWhile;
    if (Loop) {
      Into(S.Body,
           S.Kind == StmtKind::While && S.LoopHeaderAddr
               ? std::optional<va_t>(S.LoopHeaderAddr)
               : std::nullopt,
           true);
      return;
    }
    Into(S.Body, Next, false);
    Into(S.ElseBody, Next, false);
    for (SwitchCase &Case : S.Cases)
      Into(Case.Body, std::nullopt, false);
    Into(S.DefaultBody, std::nullopt, false);
  }

  void placeList(std::vector<HighStmt> Stmts, Place Around,
                 std::optional<va_t> After, bool InLoop,
                 std::vector<HighStmt> &Out) {
    const unsigned Self = NextList++;
    const std::vector<Group> Groups = groupsOf(Stmts, Around);
    // The previous group's list and end, when it falls into the next one.
    std::vector<HighStmt> *Falling = nullptr;
    size_t FallingEnd = 0;
    bool PreviousMoved = false;
    for (size_t I = 0; I < Groups.size(); ++I) {
      const Group &G = Groups[I];
      // Nothing runs what does nothing behind what ended, as the path past the
      // `call` of a body that never comes back, but for where a runtime entry
      // comes in, which no jump says: an arm can start with a local unwind.
      if (I && !Falling && !PreviousMoved && G.DoesNothing &&
          !(G.Label && Targets.count(*G.Label)) &&
          !startsRuntimeEntry(Stmts, Groups, I))
        continue;
      Place At = Tree.normal(G.At);
      // What does nothing can stay in the statement around it, unless what ran
      // before it had to leave that statement.
      if (Around != kFunctionPlace && G.DoesNothing && !PreviousMoved)
        At = Around;
      // A jump structuring lifted out of the arms before it runs after them
      // all, whatever level its address had in one, and need not go anywhere to
      // reach what is not in that scope.
      if (Falling == &Out && FallingEnd == Out.size() && G.DoesNothing &&
          Stmts[G.Begin].Kind == StmtKind::Goto)
        if (const Stretch *To = stretchAt(Stmts[G.Begin].GotoTarget);
            To && !Tree.within(Tree.ofStretch(*To), At))
          At = Around;
      bool Move = belongsElsewhere(At, Around, Self);
      const std::optional<va_t> Next = nextLabel(Stmts, G.End, After);
      if (Move) {
        bool Binds = false;
        for (size_t K = G.Begin; K < G.End; ++K)
          Binds |= bindsToALoop(Stmts[K]);
        if (Binds || (Falling && !G.Label) || (!G.Ends && !Next))
          Move = false;
      }
      if (Move) {
        if (Falling)
          Falling->insert(Falling->begin() + static_cast<ptrdiff_t>(FallingEnd),
                          jumpTo(*G.Label));
        MovedCode Code;
        Code.At = At;
        Code.Stmts.assign(std::make_move_iterator(
                              Stmts.begin() + static_cast<ptrdiff_t>(G.Begin)),
                          std::make_move_iterator(
                              Stmts.begin() + static_cast<ptrdiff_t>(G.End)));
        if (!G.Ends)
          Code.Stmts.push_back(jumpTo(*Next));
        Moved.push_back(std::move(Code));
        Falling = nullptr;
        PreviousMoved = true;
        continue;
      }
      if (belongsElsewhere(At, Around, Self))
        At = Around;
      PreviousMoved = false;
      std::vector<HighStmt> &Into = destination(At, Around, Self, Out);
      // Running on into a `__finally` is how the try ends.
      if (Falling && isClause(At) && In.Scopes[scopeOf(At)].IsFinally &&
          Falling == &Builds[scopeOf(At)].Body)
        Falling = nullptr;
      if (Falling && (Falling != &Into || FallingEnd != Into.size()) && G.Label)
        Falling->insert(Falling->begin() + static_cast<ptrdiff_t>(FallingEnd),
                        jumpTo(*G.Label));
      for (size_t K = G.Begin; K < G.End; ++K) {
        placeChildren(Stmts[K], Tree.normal(At),
                      nextLabel(Stmts, K + 1, After));
        Into.push_back(std::move(Stmts[K]));
      }
      Falling = G.Ends ? nullptr : &Into;
      FallingEnd = Into.size();
    }
    // Off the end of the list, past whatever went elsewhere.
    if (!Falling || (Falling == &Out && FallingEnd == Out.size()))
      return;
    if (After) {
      Falling->insert(Falling->begin() + static_cast<ptrdiff_t>(FallingEnd),
                      jumpTo(*After));
    } else if (InLoop) {
      HighStmt Round;
      Round.Kind = StmtKind::Continue;
      Falling->insert(Falling->begin() + static_cast<ptrdiff_t>(FallingEnd),
                      std::move(Round));
    }
  }

  void assemble(std::vector<HighStmt> &List) {
    for (HighStmt &S : List) {
      if (S.Kind == StmtKind::SEHTry) {
        Region &Built = Builds[static_cast<size_t>(S.EHClauses.front().State)];
        S.Body = std::move(Built.Body);
        S.EHClauseBodies.front() = std::move(Built.Clause);
      }
      assemble(S.Body);
      assemble(S.ElseBody);
      for (SwitchCase &Case : S.Cases)
        assemble(Case.Body);
      assemble(S.DefaultBody);
      for (std::vector<HighStmt> &Clause : S.EHClauseBodies)
        assemble(Clause);
    }
  }
};

/// Whether control can come out of \p S's end.
bool endsFlow(const HighStmt &S);

bool listEndsFlow(const std::vector<HighStmt> &List) {
  for (size_t I = List.size(); I-- > 0;) {
    if (isEmptyAnchor(List[I]) && !hasLabel(List[I]))
      continue;
    return endsFlow(List[I]);
  }
  return false;
}

bool endsFlow(const HighStmt &S) {
  if (highStmtEndsItsBlock(S))
    return true;
  if (S.Kind != StmtKind::SEHTry || !listEndsFlow(S.Body))
    return false;
  // `__leave` goes on past the try.
  bool Leaves = false;
  walkStmts(S.Body, [&](const HighStmt &Inner) { Leaves |= Inner.LeavesTry; });
  if (Leaves)
    return false;
  for (size_t K = 0; K < S.EHClauses.size(); ++K)
    if (S.EHClauses[K].Kind == HighEHClauseKind::SEHExcept &&
        !listEndsFlow(S.EHClauseBodies[K]))
      return false;
  return true;
}

/// Whether nothing runs past \p S, as past a `__try` whose `__finally` always
/// leaves.
bool nothingRunsPast(const HighStmt &S) {
  if (endsFlow(S))
    return true;
  if (S.Kind != StmtKind::SEHTry || !S.EHIsReducible)
    return false;
  for (size_t K = 0; K < S.EHClauses.size(); ++K)
    if (S.EHClauses[K].Kind == HighEHClauseKind::SEHFinally &&
        listEndsFlow(S.EHClauseBodies[K]))
      return true;
  return false;
}

/// Where \p S goes on from when its last statement is a jump.
std::optional<va_t> exitJump(const HighStmt &S) {
  if (S.Kind == StmtKind::Goto)
    return S.GotoTarget;
  if (S.Kind != StmtKind::SEHTry)
    return std::nullopt;
  for (size_t I = S.Body.size(); I-- > 0;) {
    if (isEmptyAnchor(S.Body[I]) && !hasLabel(S.Body[I]))
      continue;
    return exitJump(S.Body[I]);
  }
  return std::nullopt;
}

using TryChain = std::vector<std::pair<const HighStmt *, size_t>>;

/// Calls \p OnLabel and \p OnJump with each statement's tries, outermost first.
template <typename LabelFn, typename JumpFn>
void walkChains(const std::vector<HighStmt> &List, TryChain &Around,
                const LabelFn &OnLabel, const JumpFn &OnJump) {
  for (const HighStmt &S : List) {
    if (hasLabel(S))
      OnLabel(S, Around);
    if (S.Kind == StmtKind::Goto && !S.LeavesTry)
      OnJump(S, Around);
    const bool Try = S.Kind == StmtKind::SEHTry && S.EHIsReducible;
    if (Try)
      Around.emplace_back(&S, 0);
    walkChains(S.Body, Around, OnLabel, OnJump);
    if (Try)
      Around.pop_back();
    walkChains(S.ElseBody, Around, OnLabel, OnJump);
    for (const SwitchCase &Case : S.Cases)
      walkChains(Case.Body, Around, OnLabel, OnJump);
    walkChains(S.DefaultBody, Around, OnLabel, OnJump);
    for (size_t K = 0; K < S.EHClauseBodies.size(); ++K) {
      if (Try)
        Around.emplace_back(&S, K + 1);
      walkChains(S.EHClauseBodies[K], Around, OnLabel, OnJump);
      if (Try)
        Around.pop_back();
    }
  }
}

/// Lays out the statements of each list so that what a try's jumps enter comes
/// first in it and what a jump goes on to comes next, where nothing else falls
/// into that.
class Layout {
public:
  explicit Layout(std::vector<HighStmt> &Function) {
    std::map<va_t, TryChain> Labels;
    std::vector<std::pair<va_t, TryChain>> Jumps;
    TryChain Around;
    walkChains(
        Function, Around,
        [&](const HighStmt &S, const TryChain &Chain) {
          Labels.try_emplace(S.Addr, Chain);
        },
        [&](const HighStmt &S, const TryChain &Chain) {
          Jumps.emplace_back(S.GotoTarget, Chain);
        });
    for (const auto &[Target, From] : Jumps) {
      const auto Label = Labels.find(Target);
      if (Label == Labels.end())
        continue;
      const TryChain &To = Label->second;
      size_t Same = 0;
      while (Same < To.size() && Same < From.size() && To[Same] == From[Same])
        ++Same;
      for (size_t I = Same; I < To.size(); ++I)
        if (To[I].second == 0)
          Entries[To[I].first->EHClauses.front().HandlerVA].insert(Target);
    }
    layOut(Function, std::nullopt, 0);
  }

private:
  std::map<va_t, std::set<va_t>> Entries;

  /// The runtime enters a clause at its handler, so that has to come first in
  /// it, whatever order the statements were found in.  What falls into the
  /// handler's first statement stays before it, and a jump says the entry.
  static void startAtHandler(std::vector<HighStmt> &Clause, va_t Handler) {
    std::optional<size_t> Entry;
    va_t Lowest = InvalidVA;
    for (size_t I = 0; I < Clause.size(); ++I) {
      const auto Label = firstLabel(Clause[I]);
      if (Label && *Label >= Handler && *Label < Lowest) {
        Lowest = *Label;
        Entry = I;
      }
    }
    if (!Entry || *Entry == 0)
      return;
    size_t Begin = *Entry;
    while (Begin > 0 && !endsFlow(Clause[Begin - 1]))
      --Begin;
    if (Begin == 0) {
      Clause.insert(Clause.begin(), jumpTo(Lowest));
      return;
    }
    size_t End = *Entry;
    while (End < Clause.size() && !endsFlow(Clause[End]))
      ++End;
    End = std::min(End + 1, Clause.size());
    // What runs off the end of the clause has to stay at the end.
    if (End == Clause.size() && !endsFlow(Clause.back())) {
      Clause.insert(Clause.begin(), jumpTo(Lowest));
      return;
    }
    std::rotate(Clause.begin(), Clause.begin() + static_cast<ptrdiff_t>(Begin),
                Clause.begin() + static_cast<ptrdiff_t>(End));
  }

  /// \p Entry is where a try's body starts when jumps from outside say, and
  /// \p Handler that try's.
  void layOut(std::vector<HighStmt> &List, std::optional<va_t> Entry,
              va_t Handler) {
    for (HighStmt &S : List) {
      std::optional<va_t> Inner;
      if (S.Kind == StmtKind::SEHTry && S.EHIsReducible) {
        const auto It = Entries.find(S.EHClauses.front().HandlerVA);
        if (It != Entries.end() && It->second.size() == 1)
          Inner = *It->second.begin();
      }
      layOut(S.Body, Inner,
             S.Kind == StmtKind::SEHTry ? S.EHClauses.front().HandlerVA : 0);
      layOut(S.ElseBody, std::nullopt, 0);
      for (SwitchCase &Case : S.Cases)
        layOut(Case.Body, std::nullopt, 0);
      layOut(S.DefaultBody, std::nullopt, 0);
      for (size_t K = 0; K < S.EHClauseBodies.size(); ++K) {
        std::vector<HighStmt> &Clause = S.EHClauseBodies[K];
        startAtHandler(Clause, S.EHClauses[K].HandlerVA);
        layOut(Clause, std::nullopt, 0);
      }
    }
    struct Run {
      size_t Begin;
      size_t End;
    };
    std::vector<Run> Runs;
    for (size_t I = 0; I < List.size();) {
      size_t End = I;
      while (End < List.size() && !endsFlow(List[End]))
        ++End;
      End = std::min(End + 1, List.size());
      Runs.push_back({I, End});
      I = End;
    }
    if (Runs.size() < 2)
      return;
    // The last can run off the end of the list, and then stays last.
    bool LastFalls = !endsFlow(List.back());
    std::map<va_t, size_t> RunAt;
    for (size_t R = 0; R < Runs.size(); ++R) {
      for (size_t K = Runs[R].Begin; K < Runs[R].End; ++K) {
        if (const auto Label = firstLabel(List[K]))
          RunAt.try_emplace(*Label, R);
        if (!isEmptyAnchor(List[K]))
          break;
      }
    }
    std::vector<bool> Placed(Runs.size(), false);
    std::vector<size_t> Order;
    const size_t Final = Runs.size() - 1;
    auto Usable = [&](size_t R) {
      return !Placed[R] &&
             (!LastFalls || R != Final || Order.size() + 1 == Runs.size());
    };
    size_t Next = 0;
    if (Entry)
      if (const auto It = RunAt.find(*Entry); It != RunAt.end()) {
        Next = It->second;
        // What jumps entered runs off the end of the body, and the rest has to
        // go after it: that end is the try's normal one, which C can say.
        if (Next == Final && LastFalls) {
          HighStmt Leave = jumpTo(Handler);
          Leave.LeavesTry = true;
          List.push_back(std::move(Leave));
          ++Runs[Final].End;
          LastFalls = false;
        }
      }
    for (;;) {
      Placed[Next] = true;
      Order.push_back(Next);
      if (Order.size() == Runs.size())
        break;
      std::optional<size_t> Chosen;
      const HighStmt &Last = List[Runs[Next].End - 1];
      if (const auto Exit = exitJump(Last))
        if (const auto It = RunAt.find(*Exit);
            It != RunAt.end() && Usable(It->second)) {
          Chosen = It->second;
          // A try has to run on to what it leaves to.  Otherwise what else
          // jumps there goes first, which leaves no jump backward.
          if (Last.Kind != StmtKind::SEHTry)
            for (size_t R = 0; R < Runs.size(); ++R)
              if (R != It->second && Usable(R) &&
                  exitJump(List[Runs[R].End - 1]) == *Exit) {
                Chosen = R;
                break;
              }
        }
      for (size_t R = 0; !Chosen && R < Runs.size(); ++R)
        if (Usable(R))
          Chosen = R;
      Next = *Chosen;
    }
    std::vector<HighStmt> Laid;
    Laid.reserve(List.size());
    for (size_t R : Order)
      for (size_t K = Runs[R].Begin; K < Runs[R].End; ++K)
        Laid.push_back(std::move(List[K]));
    List = std::move(Laid);
  }
};

/// Gives each try the label of its first statement when a jump from outside
/// needs it.  A jump in the try to that statement stays in the try, as a loop
/// around the whole body does, so the two can need a label each.
class TryLabels {
public:
  explicit TryLabels(std::vector<HighStmt> &Function) : Function(Function) {
    walkStmts(Function, [&](const HighStmt &S) {
      if (hasLabel(S))
        Used.insert(S.Addr);
      if (S.Kind == StmtKind::Goto) {
        Used.insert(S.GotoTarget);
        ++Jumps[S.GotoTarget];
      }
    });
    label(Function);
  }

private:
  std::vector<HighStmt> &Function;
  std::set<va_t> Used;
  std::map<va_t, unsigned> Jumps;

  void label(std::vector<HighStmt> &List) {
    for (HighStmt &S : List) {
      label(S.Body);
      label(S.ElseBody);
      for (SwitchCase &Case : S.Cases)
        label(Case.Body);
      label(S.DefaultBody);
      for (std::vector<HighStmt> &Clause : S.EHClauseBodies)
        label(Clause);
      if (S.Kind == StmtKind::SEHTry)
        labelTry(S);
    }
  }

  /// A try's one entry can lie where layout could not bring it to the front, as
  /// in a loop the try holds: the try gets a label of its own, and its first
  /// statement jumps there, which C can say.
  bool enterByAJump(HighStmt &Try, const std::optional<va_t> &First) {
    std::set<va_t> Labels;
    std::map<va_t, unsigned> FromInside;
    std::set<const HighStmt *> InsideJumps;
    walkStmts(Try.Body, [&](const HighStmt &S) {
      if (hasLabel(S))
        Labels.insert(S.Addr);
      if (S.Kind == StmtKind::Goto) {
        ++FromInside[S.GotoTarget];
        InsideJumps.insert(&S);
      }
    });
    std::set<va_t> Entries;
    for (va_t Label : Labels)
      if (Jumps[Label] > FromInside[Label])
        Entries.insert(Label);
    if (Entries.size() != 1 || (First && *Entries.begin() == *First))
      return false;
    const va_t Entry = *Entries.begin();
    va_t Own = Entry + 1;
    while (Used.count(Own))
      ++Own;
    Used.insert(Own);
    walkStmts(Function, [&](HighStmt &S) {
      if (S.Kind == StmtKind::Goto && S.GotoTarget == Entry &&
          !InsideJumps.count(&S)) {
        S.GotoTarget = Own;
        ++Jumps[Own];
        --Jumps[Entry];
      }
    });
    ++Jumps[Entry];
    Try.Addr = Own;
    Try.Body.insert(Try.Body.begin(), jumpTo(Entry));
    return true;
  }

  void labelTry(HighStmt &Try) {
    const auto First = firstLabel(Try);
    va_t Begin = InvalidVA;
    va_t End = 0;
    std::vector<HighStmt *> Inside;
    walkStmts(Try.Body, [&](HighStmt &S) {
      if (hasLabel(S)) {
        Begin = std::min(Begin, S.Addr);
        End = std::max(End, S.Addr + 1);
      }
      if (First && S.Kind == StmtKind::Goto && S.GotoTarget == *First)
        Inside.push_back(&S);
    });
    const va_t Handler = Try.EHClauses.front().HandlerVA;
    Try.EHRange = End ? ExceptionAddressRange{Begin, End}
                      : ExceptionAddressRange{Handler, Handler + 1};
    if (enterByAJump(Try, First))
      return;
    if (!First)
      return;
    if (Jumps[*First] == Inside.size())
      return;
    Try.Addr = *First;
    if (Inside.empty())
      return;
    // The jumps inside take a label of their own, one no code has.
    va_t Own = *First + 1;
    while (Used.count(Own))
      ++Own;
    Used.insert(Own);
    for (HighStmt *Jump : Inside)
      Jump->GotoTarget = Own;
    Jumps[*First] -= static_cast<unsigned>(Inside.size());
    Jumps[Own] = static_cast<unsigned>(Inside.size());
    HighStmt Anchor;
    Anchor.Kind = StmtKind::Block;
    Anchor.Addr = Own;
    Try.Body.insert(Try.Body.begin(), std::move(Anchor));
  }
};

std::set<va_t> whatRunsNext(const std::vector<HighStmt> &List, size_t I,
                            const std::set<va_t> &After) {
  std::set<va_t> Next;
  for (size_t J = I + 1; J < List.size(); ++J) {
    const HighStmt &S = List[J];
    if (hasLabel(S))
      Next.insert(S.Addr);
    if (S.Kind == StmtKind::While && S.LoopHeaderAddr)
      Next.insert(S.LoopHeaderAddr);
    if (S.Kind == StmtKind::Goto) {
      if (!S.AbnormalExit)
        Next.insert(S.GotoTarget);
      return Next;
    }
    if (!isEmptyAnchor(S))
      return Next;
  }
  Next.insert(After.begin(), After.end());
  return Next;
}

/// Whether \p S in a jump out of a `__try` body says the normal end of the try.
void markLeaves(std::vector<HighStmt> &List, const std::set<va_t> &Next) {
  for (HighStmt &S : List) {
    if (S.Kind == StmtKind::Goto && !S.AbnormalExit && Next.count(S.GotoTarget))
      S.LeavesTry = true;
    // `__leave` in a nested try would leave that one, but its clause is here.
    if (S.Kind != StmtKind::SEHTry)
      markLeaves(S.Body, Next);
    markLeaves(S.ElseBody, Next);
    for (SwitchCase &Case : S.Cases)
      markLeaves(Case.Body, Next);
    markLeaves(S.DefaultBody, Next);
    for (size_t K = 0; K < S.EHClauseBodies.size(); ++K)
      if (S.EHClauses[K].Kind == HighEHClauseKind::SEHExcept)
        markLeaves(S.EHClauseBodies[K], Next);
  }
}

void dropJumpsIn(std::vector<HighStmt> &List, const std::set<va_t> &After) {
  for (size_t I = List.size(); I-- > 0;) {
    const std::set<va_t> Next = whatRunsNext(List, I, After);
    HighStmt &S = List[I];
    switch (S.Kind) {
    case StmtKind::SEHTry:
      dropJumpsIn(S.Body, Next);
      for (size_t K = 0; K < S.EHClauseBodies.size(); ++K)
        // A jump out of a `__finally` stops an unwind, wherever it lands.
        dropJumpsIn(S.EHClauseBodies[K],
                    S.EHClauses[K].Kind == HighEHClauseKind::SEHFinally
                        ? std::set<va_t>{}
                        : Next);
      if (S.EHIsReducible)
        markLeaves(S.Body, Next);
      break;
    case StmtKind::Block:
    case StmtKind::If:
    case StmtKind::IfElse:
      dropJumpsIn(S.Body, Next);
      dropJumpsIn(S.ElseBody, Next);
      break;
    case StmtKind::While:
    case StmtKind::For:
    case StmtKind::DoWhile: {
      std::set<va_t> Round;
      if (S.Kind == StmtKind::While && S.LoopHeaderAddr)
        Round.insert(S.LoopHeaderAddr);
      dropJumpsIn(S.Body, Round);
      break;
    }
    case StmtKind::Switch:
      for (SwitchCase &Case : S.Cases)
        dropJumpsIn(Case.Body, {});
      dropJumpsIn(S.DefaultBody, {});
      break;
    default:
      break;
    }
    if (S.Kind != StmtKind::Goto || !Next.count(S.GotoTarget))
      continue;
    // Leaving abnormally is not leaving by the end, but a `return` leaves so
    // too.
    if (S.AbnormalExit) {
      size_t Following = I + 1;
      while (Following < List.size() && isEmptyAnchor(List[Following]))
        ++Following;
      if (Following == List.size() || List[Following].Kind != StmtKind::Return)
        continue;
    }
    // Other jumps can name what it was.
    if (hasLabel(S)) {
      const va_t Addr = S.Addr;
      S = HighStmt{};
      S.Kind = StmtKind::Block;
      S.Addr = Addr;
    } else {
      List.erase(List.begin() + static_cast<ptrdiff_t>(I));
    }
  }
}

/// Jumps to a jump go where it goes, and to a `return` of nothing to be read
/// are that `return`.  The epilogue's restore of `FS:[0]` before it is the
/// runtime's business, which no C says.
class JumpThreader {
public:
  explicit JumpThreader(std::vector<HighStmt> &Function) {
    index(Function);
    apply(Function);
  }

private:
  struct Site {
    const std::vector<HighStmt> *List;
    size_t Index;
  };
  std::map<va_t, Site> Sites;

  void index(const std::vector<HighStmt> &List) {
    for (size_t I = 0; I < List.size(); ++I) {
      const HighStmt &S = List[I];
      if (hasLabel(S))
        Sites.try_emplace(S.Addr, Site{&List, I});
      index(S.Body);
      index(S.ElseBody);
      for (const SwitchCase &Case : S.Cases)
        index(Case.Body);
      index(S.DefaultBody);
      for (const std::vector<HighStmt> &Clause : S.EHClauseBodies)
        index(Clause);
    }
  }

  static bool writesRuntimeChain(const HighStmt &S) {
    return (S.Kind == StmtKind::Store &&
            S.MemoryAddressSpace == NdMemoryAddressSpace::X86FS) ||
           (S.Kind == StmtKind::Assign && S.Dst &&
            S.Dst->Kind == ExprKind::Load &&
            S.Dst->MemoryAddressSpace == NdMemoryAddressSpace::X86FS);
  }

  /// What runs from \p Target on, when it is one jump or `return` past what no
  /// C says.
  const HighStmt *leaveAt(va_t Target) const {
    const auto At = Sites.find(Target);
    if (At == Sites.end())
      return nullptr;
    const std::vector<HighStmt> &List = *At->second.List;
    for (size_t I = At->second.Index; I < List.size(); ++I) {
      const HighStmt &S = List[I];
      if (isEmptyAnchor(S) || writesRuntimeChain(S))
        continue;
      // The read of the chain's old head: a label can hold it in a block.
      const bool ReadsTheHead =
          S.Kind == StmtKind::Assign ||
          (S.Kind == StmtKind::Block &&
           llvm::all_of(S.Body, [](const HighStmt &Inner) {
             return Inner.Kind == StmtKind::Assign;
           }));
      if (ReadsTheHead && I + 1 < List.size() &&
          writesRuntimeChain(List[I + 1]))
        continue;
      if (S.Kind == StmtKind::Goto)
        return &S;
      if (S.Kind == StmtKind::Return &&
          (!S.RetVal || S.RetVal->Kind == ExprKind::Const ||
           S.RetVal->Kind == ExprKind::Undef))
        return &S;
      return nullptr;
    }
    return nullptr;
  }

  void apply(std::vector<HighStmt> &List) {
    for (HighStmt &S : List) {
      if (S.Kind == StmtKind::Goto && !S.LeavesTry) {
        // Bounded: the jumps of a loop that does nothing lead round.
        for (size_t Step = 0; Step < Sites.size(); ++Step) {
          const HighStmt *Leave = leaveAt(S.GotoTarget);
          if (!Leave || Leave == &S ||
              (Leave->Kind == StmtKind::Goto &&
               Leave->GotoTarget == S.GotoTarget))
            break;
          if (Leave->Kind == StmtKind::Return) {
            const va_t Addr = S.Addr;
            S = *Leave;
            S.Addr = Addr;
            break;
          }
          S.AbnormalExit |= Leave->AbnormalExit;
          S.GotoTarget = Leave->GotoTarget;
        }
      }
      apply(S.Body);
      apply(S.ElseBody);
      for (SwitchCase &Case : S.Cases)
        apply(Case.Body);
      apply(S.DefaultBody);
      for (std::vector<HighStmt> &Clause : S.EHClauseBodies)
        apply(Clause);
    }
  }
};

bool isLanded(const HighStmt &S, const std::set<va_t> &Targets) {
  return (hasLabel(S) && Targets.count(S.Addr)) ||
         (S.Kind == StmtKind::While && S.LoopHeaderAddr &&
          Targets.count(S.LoopHeaderAddr));
}

bool doesNothingAtAll(const HighStmt &S) {
  return isEmptyAnchor(S) || S.Kind == StmtKind::Goto ||
         (S.Kind == StmtKind::Return &&
          (!S.RetVal || S.RetVal->Kind == ExprKind::Const ||
           S.RetVal->Kind == ExprKind::Undef));
}

/// Remove what no jump lands on behind what nothing runs past.
void dropUnreached(std::vector<HighStmt> &List, const std::set<va_t> &Targets) {
  for (size_t I = 0; I < List.size();) {
    HighStmt &S = List[I];
    dropUnreached(S.Body, Targets);
    dropUnreached(S.ElseBody, Targets);
    for (SwitchCase &Case : S.Cases)
      dropUnreached(Case.Body, Targets);
    dropUnreached(S.DefaultBody, Targets);
    for (std::vector<HighStmt> &Clause : S.EHClauseBodies)
      dropUnreached(Clause, Targets);
    ++I;
    if (!nothingRunsPast(S))
      continue;
    // Only what does nothing: code a path this cannot see might still reach.
    size_t End = I;
    while (End < List.size() && !isLanded(List[End], Targets) &&
           doesNothingAtAll(List[End]))
      ++End;
    List.erase(List.begin() + static_cast<ptrdiff_t>(I),
               List.begin() + static_cast<ptrdiff_t>(End));
  }
}

} // namespace

std::vector<HighStmt>
placeRegistrationRegions(std::vector<HighStmt> Stmts,
                         const RegistrationRegionFacts &In) {
  std::vector<HighStmt> Placed = RegionPlacer(In).run(std::move(Stmts));
  Layout{Placed};
  TryLabels{Placed};
  return Placed;
}

void dropNaturalJumps(std::vector<HighStmt> &Stmts) { dropJumpsIn(Stmts, {}); }

void threadJumps(std::vector<HighStmt> &Stmts) {
  JumpThreader{Stmts};
  const std::set<va_t> Targets = gotoTargets(Stmts);
  dropUnreached(Stmts, Targets);
}

void keepRegistrationLabelsInReach(std::vector<HighStmt> &Stmts) {
  // Cleanup copies short tails, and each copy keeps its address, of which C
  // prints the first in the text as the label.
  std::map<va_t, std::vector<std::pair<const HighStmt *, TryChain>>> Labels;
  std::map<va_t, std::vector<TryChain>> Jumps;
  TryChain Around;
  walkChains(
      Stmts, Around,
      [&](const HighStmt &S, const TryChain &Chain) {
        Labels[S.Addr].emplace_back(&S, Chain);
      },
      [&](const HighStmt &S, const TryChain &Chain) {
        Jumps[S.GotoTarget].push_back(Chain);
      });
  for (auto &[Address, Sites] : Labels) {
    const auto From = Jumps.find(Address);
    if (Sites.size() < 2 || From == Jumps.end())
      continue;
    const auto Reached = [&, &Jumpers = From->second](const auto &Site) {
      return llvm::all_of(Jumpers, [&](const TryChain &Chain) {
        return Site.second.size() <= Chain.size() &&
               std::equal(Site.second.begin(), Site.second.end(),
                          Chain.begin());
      });
    };
    const auto Good = llvm::find_if(Sites, Reached);
    if (Good == Sites.end())
      continue;
    for (auto Site = Sites.begin(); Site != Good; ++Site)
      const_cast<HighStmt *>(Site->first)->Addr = 0;
  }
}

std::optional<va_t> registrationTryEntered(const std::vector<HighStmt> &Stmts) {
  // An address can name several statements, as a try's names its first: a jump
  // needs one it can reach.
  std::map<va_t, std::vector<TryChain>> Labels;
  std::vector<std::pair<va_t, TryChain>> Jumps;
  TryChain Around;
  walkChains(
      Stmts, Around,
      [&](const HighStmt &S, const TryChain &Chain) {
        Labels[S.Addr].push_back(Chain);
      },
      [&](const HighStmt &S, const TryChain &Chain) {
        Jumps.emplace_back(S.GotoTarget, Chain);
      });
  for (const auto &[Target, From] : Jumps) {
    const auto Label = Labels.find(Target);
    if (Label == Labels.end() ||
        llvm::any_of(Label->second, [&, &From = From](const TryChain &To) {
          return To.size() <= From.size() &&
                 std::equal(To.begin(), To.end(), From.begin());
        }))
      continue;
    const TryChain &To = Label->second.front();
    for (size_t I = 0; I < To.size(); ++I)
      if (I >= From.size() || To[I] != From[I])
        return To[I].first->EHClauses.front().HandlerVA;
  }
  return std::nullopt;
}

} // namespace neverd
