//===- Exceptions.cpp - CPython 3.10 exceptional control regions ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recognize protected regions only when the normal and exceptional cleanup
/// paths agree. Preserve suppression by emitting the actual with statement.
//===----------------------------------------------------------------------===//

#include "SourceDetail.h"

#include "llvm/ADT/ScopeExit.h"

using namespace neverd::python;

size_t Emitter::exceptionBlock(size_t P, size_t End, unsigned Depth, Stack &S) {
  size_t H = target(P);
  if (H <= P + 1 || H >= End)
    reject("Handler outside its enclosing region");
  auto Jump = [&](size_t N) {
    return at(N, Op::JUMP_FORWARD) || at(N, Op::JUMP_ABSOLUTE);
  };
  if (at(P, Op::SETUP_WITH)) {
    auto Manager = text(pop(S));
    if (!S.empty())
      reject("With statement crosses an operand boundary");
    std::string Binding;
    if (at(P + 1, Op::STORE_FAST) || at(P + 1, Op::STORE_NAME))
      Binding = " as " + name(F.Code[P + 1]);
    else if (!at(P + 1, Op::POP_TOP))
      reject("Unqualified context-manager binding");
    if (H + 8 > End || !at(H, Op::WITH_EXCEPT_START) ||
        !at(H + 1, Op::POP_JUMP_IF_TRUE) || target(H + 1) != H + 3 ||
        !at(H + 2, Op::RERAISE) || F.Code[H + 2].Arg != 1 ||
        !at(H + 3, Op::POP_TOP) || !at(H + 4, Op::POP_TOP) ||
        !at(H + 5, Op::POP_TOP) || !at(H + 6, Op::POP_EXCEPT) ||
        !at(H + 7, Op::POP_TOP))
      reject("Unqualified context-manager exception exit");
    size_t Leave = P + 2;
    for (; Leave + 5 < H; ++Leave) {
      if (!at(Leave, Op::POP_BLOCK) || !at(Leave + 1, Op::LOAD_CONST) ||
          C.Consts->Items[F.Code[Leave + 1].Arg]->Type != Object::None ||
          !at(Leave + 2, Op::DUP_TOP) || !at(Leave + 3, Op::DUP_TOP) ||
          !at(Leave + 4, Op::CALL_FUNCTION) || F.Code[Leave + 4].Arg != 3 ||
          !at(Leave + 5, Op::POP_TOP))
        continue;
      break;
    }
    if (Leave + 5 >= H)
      reject("Missing context-manager normal cleanup");
    size_t Normal = Leave + 6, After = H + 8;
    if (!(Normal + 1 == H && Jump(Normal) && target(Normal) == After) &&
        !(Normal < H && H - Normal <= End - After &&
          same(Normal, After, H - Normal)))
      reject("Context-manager continuations differ");
    line(Depth, "with " + Manager + Binding + ":");
    auto Before = NextLine;
    block(P + 2, Leave, Depth + 1);
    if (Before == NextLine)
      line(Depth + 1, "pass");
    return After;
  }
  if (!S.empty())
    reject("Try statement crosses an operand boundary");
  if (at(H, Op::DUP_TOP)) {
    if (H < 2 || !at(H - 2, Op::POP_BLOCK) || !Jump(H - 1) ||
        !(at(H + 1, Op::LOAD_GLOBAL) || at(H + 1, Op::LOAD_NAME)) ||
        !at(H + 2, Op::JUMP_IF_NOT_EXC_MATCH) || !at(H + 3, Op::POP_TOP) ||
        !at(H + 4, Op::POP_TOP) || !at(H + 5, Op::POP_TOP))
      reject("Unqualified typed exception handler");
    size_t Miss = target(H + 2);
    if (Miss < H + 8 || Miss >= End || !at(Miss, Op::RERAISE) ||
        F.Code[Miss].Arg != 0 || !at(Miss - 2, Op::POP_EXCEPT) ||
        !Jump(Miss - 1))
      reject("Unqualified exception handler epilogue");
    size_t Normal = normalized(target(H - 1)),
           Handled = normalized(target(Miss - 1));
    if (Normal <= Miss || Normal > End || Handled != Normal)
      reject("Unqualified exception-handler join");
    line(Depth, "try:");
    auto Before = NextLine;
    block(P + 1, H - 2, Depth + 1);
    if (Before == NextLine)
      line(Depth + 1, "pass");
    line(Depth, "except " + name(F.Code[H + 1]) + ":");
    Before = NextLine;
    block(H + 6, Miss - 2, Depth + 1);
    if (Before == NextLine)
      line(Depth + 1, "pass");
    return Normal;
  }
  size_t Reraise = H;
  while (Reraise < End && !at(Reraise, Op::RERAISE))
    ++Reraise;
  if (Reraise == End || F.Code[Reraise].Arg != 0 || Reraise == H)
    reject("Unqualified finally handler");
  size_t Length = Reraise - H;
  std::vector<size_t> Exits;
  for (size_t N = P + 1; N < H; ++N)
    if (at(N, Op::POP_BLOCK) && Length <= H - N - 1 && same(N + 1, H, Length))
      Exits.push_back(N);
  if (Exits.empty())
    reject("Normal path lacks matching finally body");
  size_t First = Exits.front(), Tail = First + 1 + Length,
         Limit = Exits.size() > 1 ? Exits[1] : H;
  if (Limit < Tail)
    reject("Overlapping finally cleanup");
  for (size_t I = 1; I < Exits.size(); ++I) {
    size_t At = Exits[I] + 1 + Length,
           Next = I + 1 < Exits.size() ? Exits[I + 1] : H;
    if (Next < At || Next - At != Limit - Tail || !same(Tail, At, Limit - Tail))
      reject("Finally continuations differ");
    EquivalentExits.emplace(Exits[I], First);
  }
  auto Restore = llvm::scope_exit([&] {
    for (size_t I = 1; I < Exits.size(); ++I)
      EquivalentExits.erase(Exits[I]);
  });
  line(Depth, "try:");
  auto Before = NextLine;
  block(P + 1, First, Depth + 1);
  if (Before == NextLine)
    line(Depth + 1, "pass");
  line(Depth, "finally:");
  Before = NextLine;
  block(H, Reraise, Depth + 1);
  if (Before == NextLine)
    line(Depth + 1, "pass");
  size_t After = Reraise + 1;
  if (Limit > Tail && Jump(Limit - 1)) {
    if (target(Limit - 1) != After)
      reject("Finally continuation jumps outside the region");
    --Limit;
  }
  block(Tail, Limit, Depth);
  return After;
}
