//===- LuaJit.cpp - LuaJIT versioned dump records ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Length-delimited prototypes are linked by the dump's child stack.
//===----------------------------------------------------------------------===//

#include "Chunk.h"

#include <algorithm>
#include <climits>

using namespace neverd::lua;
namespace {
llvm::StringRef zeroTerminated(Reader &R) {
  auto N = R.Bytes.drop_front(R.Pos).find('\0');
  if (N == llvm::StringRef::npos)
    malformed(R.at(), "Unterminated debug name");
  return R.take(N + 1).drop_back();
}
void debug(Reader &R, Proto &P, unsigned Span) {
  for (unsigned PC = 0; PC < P.Code.size(); ++PC) {
    auto Delta = R.word(Span < 256 ? 1 : Span < 65536 ? 2 : 4);
    if (Delta > Span)
      malformed(R.at(), "Invalid debug line offset");
    P.Lines.push_back(P.FirstLine + Delta);
  }
  for (auto &Up : P.Up)
    Up.Name = zeroTerminated(R);
  uint64_t PC = 0;
  while (true) {
    auto Tag = R.byte();
    if (!Tag)
      break;
    R.count(1);
    llvm::StringRef Name;
    if (Tag > 6) {
      --R.Pos;
      Name = zeroTerminated(R);
    }
    PC += R.varint();
    auto End = PC + R.varint();
    if (End > P.Code.size() + 1)
      malformed(R.at(), "Local lifetime outside prototype");
    // The debug stream includes the VM's implicit function-header PC.
    P.Locals.push_back(
        {Name, unsigned(PC ? PC - 1 : 0), unsigned(End ? End - 1 : 0)});
  }
  R.end();
}
void tableItem(Reader &R, bool Key) {
  auto Type = R.varint();
  if (Key && Type == 0)
    malformed(R.at(), "Nil hash key");
  if (Type >= 5)
    R.take(Type - 5);
  else if (Type == 3)
    R.varint();
  else if (Type == 4) {
    R.varint();
    R.varint();
  }
}
Constant gc(Reader &R, Proto &P, std::vector<unsigned> &Stack, Chunk &C) {
  auto Type = R.varint();
  if (Type >= 5)
    return {Constant::String, 0, 0, R.take(Type - 5)};
  if (Type == 0) {
    if (Stack.empty())
      malformed(R.at(), "Missing child prototype");
    auto Id = Stack.back();
    Stack.pop_back();
    P.Children.push_back(Id);
    for (auto &Up : C.Functions[Id].Up)
      if (Up.Index >= (Up.Local ? P.Stack : P.Up.size()))
        malformed(R.at(), "Invalid closure capture");
    return {Constant::Closure, Id};
  }
  if (Type == 1) {
    auto Array = R.count(R.varint()), Hash = R.count(R.varint());
    for (unsigned I = 0; I < Array; ++I)
      tableItem(R, false);
    for (unsigned I = 0; I < Hash; ++I) {
      tableItem(R, true);
      tableItem(R, false);
    }
    return {Constant::Table};
  }
  for (unsigned I = 0; I < (Type == 4 ? 4u : 2u); ++I)
    R.varint();
  return {Constant::CData};
}
} // namespace
void neverd::lua::readJit(Chunk &C, Reader &R) {
  R.expect("\x1bLJ");
  C.Version = R.byte();
  auto &E = C.Evidence;
  E["dialect"] = "LuaJIT";
  E["dump_version"] = C.Version;
  if (C.Version != 1 && C.Version != 2)
    unsupported(R.at() - 1, "Unsupported LuaJIT dump version");
  E["reader_revision"] = C.Version == 1
                             ? "d7b8df8edd962bd5acb800c86be5ad88d407bd37"
                             : "3bfb8fa4ab970b36d42d108ead04e92341c49998";
  auto Flags = R.varint();
  E["dump_flags"] = Flags;
  if (Flags & ~(C.Version == 1 ? 7u : 31u))
    unsupported(R.at(), "Unknown LuaJIT dump flags");
  R.Big = Flags & 1;
  C.FR2 = Flags & 8;
  E["byte_order"] = R.Big ? "big" : "little";
  E["frame_slots"] = C.FR2 ? 2 : 1;
  E["gc64"] = nullptr;
  E["stripped"] = bool(Flags & 2);
  E["ffi"] = bool(Flags & 4);
  E["bitop"] = bool(Flags & 16);
  auto Name = (Flags & 2) ? llvm::StringRef() : R.take(R.varint());
  std::vector<unsigned> Stack;
  while (auto Size = R.varint()) {
    auto Body = R.slice(Size);
    auto &P = C.proto(Body);
    P.SourceName = Name;
    P.Flags = Body.byte();
    P.Vararg = P.Flags & 2;
    P.Params = Body.byte();
    P.Stack = Body.byte();
    auto NU = Body.count(Body.byte()), NG = Body.count(Body.varint()),
         NK = Body.count(Body.varint());
    auto NW = Body.varint();
    auto DebugSize = (Flags & 2) ? 0 : Body.varint();
    unsigned Span = 0;
    if (DebugSize) {
      P.FirstLine = Body.varint();
      Span = Body.varint();
      if (uint64_t(P.FirstLine) + Span > UINT_MAX)
        malformed(Body.at(), "Invalid debug line span");
      P.LastLine = P.FirstLine + Span;
    }
    C.code(P, Body, NW);
    for (unsigned I = 0; I < NU; ++I) {
      auto V = Body.word(2);
      P.Up.push_back(
          {bool(V & 0x8000), unsigned(V & 0x3fff), unsigned((V >> 14) & 1)});
    }
    for (unsigned I = 0; I < NG; ++I)
      P.GC.push_back(gc(Body, P, Stack, C));
    std::reverse(P.GC.begin(), P.GC.end());
    for (unsigned I = 0; I < NK; ++I) {
      uint64_t Lo = Body.varint(true, false, 33);
      if (Lo & 1)
        P.K.push_back(
            {Constant::Number, 0,
             std::bit_cast<double>((Body.varint() << 32) | (Lo >> 1))});
      else
        P.K.push_back({Constant::Integer, int32_t(Lo >> 1)});
    }
    if (DebugSize) {
      auto D = Body.slice(DebugSize);
      debug(D, P, Span);
    }
    Body.end();
    P.End = Body.at();
    P.Complete = true;
    Stack.push_back(P.Id);
  }
  if (Stack.size() != 1)
    malformed(R.at(), "Expected one root LuaJIT prototype");
  C.Root = Stack.back();
  R.end();
}
