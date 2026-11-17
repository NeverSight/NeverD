//===- HighFlowGraph.h - Control flow of a structured body ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The statement-level control flow of a structured HighIR body: where each
/// statement continues, as C runs it.  An exception handler's body is no
/// successor of its try; the unwinder enters it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_HIGH_PASS_HIGHFLOWGRAPH_H
#define NEVERD_LIB_IR_HIGH_PASS_HIGHFLOWGRAPH_H

#include "neverd/ir/high/HighIR.h"

#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace neverd {

/// Successors of every statement of a structured body.  Node 0 is the
/// function exit; a jump reaches the first statement that starts a label at
/// its target, and a jump to a do-while enters its body, as C does.
struct HighFlowGraph {
  struct Node {
    const HighStmt *Statement = nullptr;
    std::vector<size_t> Successors;
  };
  std::vector<Node> Nodes;
  size_t Entry = 0;
  bool Complete = true;
  /// The node of each statement.
  std::map<const HighStmt *, size_t> NodeOf;
  /// The node of the statement that starts each label.
  std::map<va_t, size_t> LabelAt;
  /// Where control goes when it runs off the end of each statement list.
  std::map<const std::vector<HighStmt> *, size_t> ListNext;

  HighFlowGraph(const std::vector<HighStmt> &Body,
                std::function<bool(const HighStmt &)> EndsPath)
      : EndsPath(std::move(EndsPath)) {
    Nodes.emplace_back();
    number(Body, 0);
    Entry = buildList(Body, 0, 0, 0);
    // Jumps resolve once every do-while knows where its body starts: the
    // loop's own node is its test, which a jump to its label does not run.
    for (const auto &[Jump, Target] : PendingJumps) {
      auto It = LabelAt.find(Target);
      if (It == LabelAt.end()) {
        Complete = false;
        continue;
      }
      auto Body = DoWhileBody.find(It->second);
      Nodes[Jump].Successors = {Body == DoWhileBody.end() ? It->second
                                                          : Body->second};
    }
  }

private:
  std::function<bool(const HighStmt &)> EndsPath;
  std::vector<std::pair<size_t, va_t>> PendingJumps;
  std::map<size_t, size_t> DoWhileBody;

  void number(const std::vector<HighStmt> &L, va_t Parent) {
    for (size_t I = 0; I < L.size(); ++I) {
      const HighStmt &S = L[I];
      const size_t N = Nodes.size();
      NodeOf[&S] = N;
      Nodes.push_back({&S, {}});
      if (S.Addr && S.Addr != InvalidVA &&
          S.Addr != (I == 0 ? Parent : L[I - 1].Addr))
        LabelAt.emplace(S.Addr, N);
      number(S.Body, S.Addr);
      number(S.ElseBody, 0);
      for (const SwitchCase &C : S.Cases)
        number(C.Body, 0);
      number(S.DefaultBody, 0);
      for (const auto &Clause : S.EHClauseBodies)
        number(Clause, 0);
    }
  }

  size_t buildList(const std::vector<HighStmt> &L, size_t Next, size_t Brk,
                   size_t Cnt) {
    ListNext[&L] = Next;
    for (size_t I = L.size(); I-- > 0;)
      Next = buildStmt(L[I], Next, Brk, Cnt);
    return Next;
  }

  /// Link \p S, which continues at \p Next; returns where it is entered.
  size_t buildStmt(const HighStmt &S, size_t Next, size_t Brk, size_t Cnt) {
    const size_t N = NodeOf[&S];
    std::vector<size_t> Succ;
    const bool Forever =
        !S.Cond || (S.Cond->Kind == ExprKind::Const && S.Cond->ConstVal != 0);
    size_t Entry = N;
    switch (S.Kind) {
    case StmtKind::Goto:
      PendingJumps.push_back({N, S.GotoTarget});
      break;
    case StmtKind::Return:
      Succ.push_back(0);
      break;
    case StmtKind::Break:
      Succ.push_back(Brk);
      break;
    case StmtKind::Continue:
      Succ.push_back(Cnt);
      break;
    case StmtKind::If:
      Succ = {buildList(S.Body, Next, Brk, Cnt), Next};
      break;
    case StmtKind::IfElse:
      Succ = {buildList(S.Body, Next, Brk, Cnt),
              buildList(S.ElseBody, Next, Brk, Cnt)};
      break;
    case StmtKind::While:
    case StmtKind::For:
      Succ.push_back(buildList(S.Body, N, Next, N));
      if (!Forever)
        Succ.push_back(Next);
      break;
    case StmtKind::DoWhile:
      Entry = buildList(S.Body, N, Next, N);
      DoWhileBody[N] = Entry;
      Succ = {Entry, Next};
      break;
    case StmtKind::Switch: {
      const size_t Default = S.DefaultBody.empty()
                                 ? Next
                                 : buildList(S.DefaultBody, Next, Next, Cnt);
      size_t Following = Default;
      for (size_t I = S.Cases.size(); I-- > 0;) {
        const SwitchCase &C = S.Cases[I];
        Following =
            buildList(C.Body, C.FallsThrough ? Following : Next, Next, Cnt);
        Succ.push_back(Following);
      }
      Succ.push_back(Default);
      break;
    }
    default:
      if (EndsPath(S)) {
        Succ.push_back(0);
      } else if (!S.Body.empty() || !S.EHClauseBodies.empty()) {
        Succ.push_back(buildList(S.Body, Next, Brk, Cnt));
        for (const auto &Clause : S.EHClauseBodies)
          buildList(Clause, Next, Brk, Cnt);
      } else {
        Succ.push_back(Next);
      }
      break;
    }
    Nodes[N].Successors = std::move(Succ);
    return Entry;
  }
};

} // namespace neverd

#endif // NEVERD_LIB_IR_HIGH_PASS_HIGHFLOWGRAPH_H
