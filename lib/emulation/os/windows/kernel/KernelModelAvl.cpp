///
/// \file
/// KernelModelAvl.
///

#include "KernelModel.h"
#include "KernelReferenceImage.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
namespace neverd::emulation {
namespace {
constexpr uint32_t TableBytes = 104, LinkBytes = 32, MaxNodes = 4096,
                   MaxData = 65536;
llvm::Error invalid(const llvm::Twine &T) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "generic AVL: " + T);
}
bool overlap(uint64_t A, uint64_t N, uint64_t B, uint64_t M = 1) {
  return N && M && A < B + M && B < A + N;
}
template <class T> unsigned height(const T &Tree, uint64_t N) {
  return N ? Tree.Nodes.at(N).Height : 0;
}
template <class T> int balance(const T &Tree, uint64_t N) {
  const auto &V = Tree.Nodes.at(N);
  return int(height(Tree, V.Right)) - int(height(Tree, V.Left));
}
template <class T> void update(T &Tree, uint64_t N) {
  auto &V = Tree.Nodes.at(N);
  V.Height = 1 + std::max(height(Tree, V.Left), height(Tree, V.Right));
}
template <class T>
void replace(T &Tree, uint64_t Parent, uint64_t Old, uint64_t New) {
  if (!Parent)
    Tree.Root = New;
  else if (Tree.Nodes.at(Parent).Left == Old)
    Tree.Nodes.at(Parent).Left = New;
  else
    Tree.Nodes.at(Parent).Right = New;
  if (New)
    Tree.Nodes.at(New).Parent = Parent;
}
template <class T> uint64_t rotate(T &Tree, uint64_t N, bool Left) {
  auto &A = Tree.Nodes.at(N);
  const auto R = Left ? A.Right : A.Left;
  auto &B = Tree.Nodes.at(R);
  const auto Middle = Left ? B.Left : B.Right;
  replace(Tree, A.Parent, N, R);
  if (Left) {
    A.Right = Middle;
    B.Left = N;
  } else {
    A.Left = Middle;
    B.Right = N;
  }
  if (Middle)
    Tree.Nodes.at(Middle).Parent = N;
  A.Parent = R;
  update(Tree, N);
  update(Tree, R);
  return R;
}
template <class T> void rebalance(T &Tree, uint64_t N) {
  while (N) {
    update(Tree, N);
    const auto B = balance(Tree, N);
    if (B < -1) {
      if (balance(Tree, Tree.Nodes.at(N).Left) > 0)
        rotate(Tree, Tree.Nodes.at(N).Left, true);
      N = rotate(Tree, N, false);
    } else if (B > 1) {
      if (balance(Tree, Tree.Nodes.at(N).Right) < 0)
        rotate(Tree, Tree.Nodes.at(N).Right, false);
      N = rotate(Tree, N, true);
    }
    N = Tree.Nodes.at(N).Parent;
  }
}
template <class T> void remove(T &Tree, uint64_t N) {
  auto &V = Tree.Nodes.at(N);
  uint64_t Start = V.Parent;
  if (!V.Left || !V.Right)
    replace(Tree, V.Parent, N, V.Left ? V.Left : V.Right);
  else {
    const bool Successor = balance(Tree, N) >= 0;
    auto R = Successor ? V.Right : V.Left;
    while (Successor ? Tree.Nodes.at(R).Left : Tree.Nodes.at(R).Right)
      R = Successor ? Tree.Nodes.at(R).Left : Tree.Nodes.at(R).Right;
    auto &Other = Tree.Nodes.at(R);
    const auto OldParent = Other.Parent;
    replace(Tree, OldParent, R, Successor ? Other.Right : Other.Left);
    replace(Tree, V.Parent, N, R);
    Other.Left = V.Left;
    Other.Right = V.Right;
    if (Other.Left)
      Tree.Nodes.at(Other.Left).Parent = R;
    if (Other.Right)
      Tree.Nodes.at(Other.Right).Parent = R;
    Start = OldParent == N ? R : OldParent;
  }
  Tree.Nodes.erase(N);
  ++Tree.Deletes;
  rebalance(Tree, Start);
}
} // namespace
llvm::Error KernelModel::writeAvlTable(uint64_t Address) {
  auto &T = AvlTables.at(Address);
  std::vector<uint8_t> B(TableBytes, 0);
  auto Q = [&](unsigned O, uint64_t V) {
    llvm::support::endian::write64le(B.data() + O, V);
  };
  auto D = [&](unsigned O, uint32_t V) {
    llvm::support::endian::write32le(B.data() + O, V);
  };
  Q(0, Address);
  Q(16, T.Root);
  D(44, T.Nodes.size());
  D(48, height(T, T.Root));
  D(64, T.Deletes);
  Q(72, T.Compare);
  Q(80, T.Allocate);
  Q(88, T.Free);
  Q(96, T.Context);
  for (const auto &[Base, N] : T.Nodes) {
    std::vector<uint8_t> Links(LinkBytes, 0);
    llvm::support::endian::write64le(Links.data(),
                                     N.Parent ? N.Parent : Address);
    llvm::support::endian::write64le(Links.data() + 8, N.Left);
    llvm::support::endian::write64le(Links.data() + 16, N.Right);
    Links[24] = uint8_t(balance(T, Base));
    if (auto E = Memory.write(Base, Links))
      return E;
  }
  if (auto E = Memory.write(Address, B))
    return E;
  T.Expected = std::move(B);
  return llvm::Error::success();
}
llvm::Error KernelModel::validateAvlTable(uint64_t Address) const {
  auto T = AvlTables.find(Address);
  if (T == AvlTables.end())
    return invalid(
        "requires a table initialized by RtlInitializeGenericTableAvl");
  if (auto E = objectBuffer(Address, TableBytes, false))
    return E;
  std::vector<uint8_t> B(TableBytes);
  if (auto E = Memory.read(Address, B))
    return E;
  if (B != T->second.Expected)
    return invalid("table metadata changed outside its RTL owner; initialize "
                   "before reuse");
  return llvm::Error::success();
}
llvm::Error KernelModel::validateAvlAccess(uint64_t Address, uint32_t Size,
                                           bool Write) const {
  for (const auto &[Base, T] : AvlTables) {
    if (Write && (T.Busy || !T.Nodes.empty()) &&
        overlap(Address, Size, Base, TableBytes))
      return invalid("write to live opaque table metadata");
    for (const auto &[N, V] : T.Nodes)
      if (overlap(Address, Size, N, LinkBytes))
        return invalid("access to opaque AVL node links");
  }
  return llvm::Error::success();
}
llvm::Error KernelModel::canReleaseAvlStorage(uint64_t Base,
                                              uint64_t Size) const {
  for (const auto &[Address, T] : AvlTables) {
    if ((T.Busy || !T.Nodes.empty()) &&
        overlap(Base, Size, Address, TableBytes))
      return invalid("cannot release a live table or callback storage");
    if (T.Busy || !T.Nodes.empty())
      for (auto P : {T.Compare, T.Allocate, T.Free, T.Context})
        if (P && overlap(Base, Size, P))
          return invalid("cannot release live AVL callback code or context");
    for (const auto &[N, V] : T.Nodes)
      if (overlap(Base, Size, N, LinkBytes + V.Size))
        return invalid("live nodes require RtlDeleteElementGenericTableAvl");
  }
  for (const auto &[ID, O] : AvlOperations) {
    if (O.Phase != AvlOperation::Stage::Free && O.Input &&
        overlap(Base, Size, O.Input))
      return invalid("cannot release input borrowed by an AVL callback");
    for (auto P : {O.Flag, O.ParentOut, O.SearchOut})
      if (P && overlap(Base, Size, P,
                       P == O.ParentOut   ? 8
                       : P == O.SearchOut ? 4
                                          : 1))
        return invalid("cannot release output borrowed by an AVL callback");
  }
  return llvm::Error::success();
}
void KernelModel::forgetEmptyAvlStorage(uint64_t Base, uint64_t Size) {
  for (auto I = AvlTables.begin(); I != AvlTables.end();) {
    if (!I->second.Busy && I->second.Nodes.empty() &&
        overlap(Base, Size, I->first, TableBytes))
      I = AvlTables.erase(I);
    else
      ++I;
  }
}
llvm::Error KernelModel::validateAvlUnload() const {
  if (!AvlOperations.empty() || PendingAvlCall)
    return invalid("unload returned with an active AVL callback");
  for (const auto &[A, T] : AvlTables)
    if (!T.Nodes.empty())
      return invalid("unload returned with live AVL nodes");
  return llvm::Error::success();
}
llvm::Expected<uint64_t> KernelModel::callAvl(llvm::StringRef Name,
                                              llvm::ArrayRef<uint64_t> A) {
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return invalid("requires windows_version 10.0.19041");
  if (Name == "RtlInitializeGenericTableAvl") {
    for (const auto &[Address, T] : AvlTables)
      if (overlap(A[0], TableBytes, Address, TableBytes) &&
          (T.Busy || !T.Nodes.empty()))
        return invalid("cannot reinitialize or overlap a live table");
    if (auto E = objectBuffer(A[0], TableBytes, true))
      return E;
    for (auto P : A.slice(1, 3)) {
      auto X = Memory.canAccess(P, 1, Execute);
      if (!X)
        return X.takeError();
      if (!P || !*X || (Exports && Exports->lookup(P)) ||
          (DebuggerReference && DebuggerReference->contains(P)) ||
          (P >= profile::ThunkBase &&
           P < profile::ThunkBase + profile::ThunkSize))
        return invalid(
            "callbacks require executable guest routines, not kernel exports");
      if (overlap(A[0], TableBytes, P))
        return invalid("table cannot overlap callback code");
    }
    forgetEmptyAvlStorage(A[0], TableBytes);
    if (AvlTables.size() >= 64)
      return invalid("table inventory exceeds 64 live storage identities");
    AvlTable T;
    T.Compare = A[1];
    T.Allocate = A[2];
    T.Free = A[3];
    T.Context = A[4];
    AvlTables.emplace(A[0], std::move(T));
    if (auto E = writeAvlTable(A[0]))
      return E;
    return 0;
  }
  if (auto E = validateAvlTable(A[0]))
    return E;
  auto &T = AvlTables.at(A[0]);
  if (Name == "RtlNumberGenericTableElementsAvl")
    return uint64_t(T.Nodes.size());
  if (Name == "RtlIsGenericTableEmptyAvl")
    return uint64_t(T.Nodes.empty());
  if (T.Busy)
    return invalid("table has an in-flight operation; recursive "
                   "lookup/mutation is unsupported");
  if (hasPendingModelGuestCall() || PendingWait)
    return invalid("cannot replace a prepared callback or wait");
  if (AvlOperations.size() >= 64 || NextAvlOperation == UINT64_MAX)
    return invalid("callback continuation capacity exhausted");
  AvlOperation O;
  O.Type =
      Name == "RtlLookupElementGenericTableFullAvl"
          ? AvlOperation::Kind::LookupFull
      : Name == "RtlLookupElementGenericTableAvl" ? AvlOperation::Kind::Lookup
      : Name == "RtlInsertElementGenericTableFullAvl"
          ? AvlOperation::Kind::InsertFull
      : Name == "RtlInsertElementGenericTableAvl" ? AvlOperation::Kind::Insert
                                                  : AvlOperation::Kind::Delete;
  O.Table = A[0];
  O.Input = A[1];
  O.Node = T.Root;
  O.Thread = CurrentThreadKey;
  O.IRQL = CurrentIRQL;
  if (O.Type == AvlOperation::Kind::LookupFull) {
    O.ParentOut = A[2];
    O.SearchOut = A[3];
    if (auto E = objectBuffer(O.ParentOut, 8, true))
      return E;
    if (auto E = objectBuffer(O.SearchOut, 4, true))
      return E;
    if (overlap(O.ParentOut, 8, O.SearchOut, 4))
      return invalid("aliased lookup outputs are unsupported");
  }
  if (O.Type == AvlOperation::Kind::Insert ||
      O.Type == AvlOperation::Kind::InsertFull) {
    if (int32_t(A[2]) < 0 || uint32_t(A[2]) > MaxData)
      return invalid("data size must be nonnegative and at most 65536 bytes");
    O.Size = uint32_t(A[2]);
    O.Flag = A[3];
    if (O.Flag) {
      if (auto E = objectBuffer(O.Flag, 1, true))
        return E;
      if (overlap(O.Flag, 1, O.Input, O.Size))
        return invalid("aliased insertion data/output are unsupported");
    }
  }
  if (O.Type == AvlOperation::Kind::InsertFull) {
    O.Search = uint32_t(A[5]);
    O.Parent = A[4];
    O.Node = 0;
    if (O.Search > 3 || (O.Search == 0 && T.Root) ||
        (O.Search && !T.Nodes.contains(O.Parent)))
      return invalid("full insertion requires a valid owned search result");
    if (!O.Search)
      O.Parent = 0;
    if ((O.Search == 2 && T.Nodes.at(O.Parent).Left) ||
        (O.Search == 3 && T.Nodes.at(O.Parent).Right))
      return invalid("full insertion position is occupied or stale");
    O.Found = O.Search == 1;
  }
  const auto ID = NextAvlOperation++;
  T.Busy = ID;
  AvlOperations.emplace(ID, std::move(O));
  auto Result = advanceAvl(ID);
  if (!Result) {
    T.Busy = 0;
    AvlOperations.erase(ID);
    return Result.takeError();
  }
  return Result->value_or(0);
}
llvm::Expected<std::optional<uint64_t>> KernelModel::advanceAvl(uint64_t ID) {
  auto &O = AvlOperations.at(ID);
  auto &T = AvlTables.at(O.Table);
  auto Complete = [&](uint64_t V) -> llvm::Expected<std::optional<uint64_t>> {
    T.Busy = 0;
    AvlOperations.erase(ID);
    return std::optional<uint64_t>{V};
  };
  auto Prepare = [&](uint64_t PC, std::vector<uint64_t> Args)
      -> llvm::Expected<std::optional<uint64_t>> {
    if (PendingAvlCall)
      return invalid("callback slot is occupied");
    O.Active = false;
    PendingAvlCall =
        KernelGuestCall{{GuestCallOwner::Avl, ID}, PC, std::move(Args), 0};
    return std::optional<uint64_t>{};
  };
  if (O.Node) {
    O.Phase = AvlOperation::Stage::Compare;
    return Prepare(T.Compare, {O.Table, O.Input, O.Node + LinkBytes});
  }
  if (O.Type == AvlOperation::Kind::Lookup ||
      O.Type == AvlOperation::Kind::LookupFull) {
    if (O.Type == AvlOperation::Kind::LookupFull) {
      if (O.Search)
        if (auto E = Memory.writeInteger(O.ParentOut, O.Parent, 8))
          return E;
      if (auto E = Memory.writeInteger(O.SearchOut, O.Search, 4))
        return E;
    }
    return Complete(O.Found ? O.Parent + LinkBytes : 0);
  }
  if (O.Type == AvlOperation::Kind::Delete) {
    if (!O.Found)
      return Complete(0);
    const auto Node = O.Parent;
    remove(T, Node);
    if (auto E = writeAvlTable(O.Table))
      return E;
    O.Phase = AvlOperation::Stage::Free;
    return Prepare(T.Free, {O.Table, Node});
  }
  if (O.Found) {
    if (O.Flag)
      if (auto E = Memory.writeInteger(O.Flag, 0, 1))
        return E;
    return Complete(O.Parent + LinkBytes);
  }
  if (T.Nodes.size() >= MaxNodes)
    return invalid("tree exceeds 4096 nodes");
  if (O.Size)
    if (auto E = objectBuffer(O.Input, O.Size, false))
      return E;
  O.Phase = AvlOperation::Stage::Allocate;
  return Prepare(T.Allocate, {O.Table, LinkBytes + O.Size});
}
llvm::Error KernelModel::beginAvlCall(uint64_t ID) {
  auto I = AvlOperations.find(ID);
  if (I == AvlOperations.end() || I->second.Active ||
      I->second.Thread != CurrentThreadKey || I->second.IRQL != CurrentIRQL)
    return invalid("callback entry lost its operation, caller thread or IRQL");
  if (auto E = validateAvlTable(I->second.Table))
    return E;
  I->second.Active = true;
  return llvm::Error::success();
}
llvm::Expected<std::optional<uint64_t>>
KernelModel::finishAvlCall(uint64_t ID, uint64_t Value) {
  auto I = AvlOperations.find(ID);
  if (I == AvlOperations.end() || !I->second.Active ||
      I->second.Thread != CurrentThreadKey || I->second.IRQL != CurrentIRQL)
    return invalid("callback return lost its operation, caller thread or IRQL");
  auto &O = I->second;
  auto &T = AvlTables.at(O.Table);
  if (auto E = validateAvlTable(O.Table))
    return E;
  O.Active = false;
  auto Complete = [&](uint64_t V) -> llvm::Expected<std::optional<uint64_t>> {
    T.Busy = 0;
    AvlOperations.erase(ID);
    return std::optional<uint64_t>{V};
  };
  if (O.Phase == AvlOperation::Stage::Free)
    return Complete(1);
  if (O.Phase == AvlOperation::Stage::Compare) {
    const auto Compared = uint32_t(Value);
    if (Compared > 2)
      return invalid("comparison callback returned an invalid ordering value");
    O.Parent = O.Node;
    O.Search = Compared == 2 ? 1 : Compared == 0 ? 2 : 3;
    O.Found = Compared == 2;
    O.Node = Compared == 2   ? 0
             : Compared == 0 ? T.Nodes.at(O.Parent).Left
                             : T.Nodes.at(O.Parent).Right;
    return advanceAvl(ID);
  }
  if (!Value) {
    if (O.Flag)
      if (auto E = Memory.writeInteger(O.Flag, 0, 1))
        return E;
    return Complete(0);
  }
  auto Allocation = Allocations.find(Value);
  const uint64_t Size = LinkBytes + O.Size;
  if (Allocation == Allocations.end() || Allocation->second.Size < Size ||
      (Value & 7))
    return invalid("allocation callback must return a distinct aligned live "
                   "kernel pool base with enough bytes");
  if (overlap(Value, Size, O.Input, O.Size) ||
      (O.Flag && overlap(Value, Size, O.Flag)))
    return invalid("allocation aliases insertion data or output");
  if (auto E = objectBuffer(Value, Size, true))
    return E;
  std::vector<uint8_t> Data(O.Size);
  if (O.Size) {
    if (auto E = objectBuffer(O.Input, O.Size, false))
      return E;
    if (auto E = Memory.read(O.Input, Data))
      return E;
    if (auto E = Memory.write(Value + LinkBytes, Data))
      return E;
  }
  AvlNode N;
  N.Parent = O.Parent;
  N.Size = O.Size;
  T.Nodes.emplace(Value, N);
  if (!O.Parent)
    T.Root = Value;
  else if (O.Search == 2)
    T.Nodes.at(O.Parent).Left = Value;
  else
    T.Nodes.at(O.Parent).Right = Value;
  rebalance(T, Value);
  if (auto E = writeAvlTable(O.Table))
    return E;
  if (O.Flag)
    if (auto E = Memory.writeInteger(O.Flag, 1, 1))
      return E;
  return Complete(Value + LinkBytes);
}
} // namespace neverd::emulation
