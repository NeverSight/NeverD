#pragma once

// This private adapter is included only by the parser translation unit.
// Never mix LLVM and Hermes' LLVH headers across this boundary.
#include "hermes/AST/ESTree.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Session.h"
#include "neverd/web/Source.h"

namespace neverd::web {
namespace hermes_model {
namespace AST = hermes::ESTree;

/// Hermes stores each UTF-16 code unit in a separate UTF-8-like sequence.
/// Decode that representation without replacing surrogate code units.
inline std::u16string codeUnits(llvh::StringRef Bytes, uint64_t &Remaining) {
  std::u16string Result;
  for (size_t I = 0; I < Bytes.size();) {
    const auto First = uint8_t(Bytes[I++]);
    uint32_t Unit = First;
    unsigned Extra = 0;
    uint32_t Minimum = 0;
    if (First >= 0xC2 && First <= 0xDF) {
      Unit = First & 0x1F;
      Extra = 1;
      Minimum = 0x80;
    } else if (First >= 0xE0 && First <= 0xEF) {
      Unit = First & 0x0F;
      Extra = 2;
      Minimum = 0x800;
    } else if (First >= 0x80) {
      throw Error("unsupported_parser_string_encoding");
    }
    while (Extra--) {
      if (I == Bytes.size() || (uint8_t(Bytes[I]) & 0xC0) != 0x80)
        throw Error("unsupported_parser_string_encoding");
      Unit = (Unit << 6) | (uint8_t(Bytes[I++]) & 0x3F);
    }
    if (Unit < Minimum || Unit > 0xFFFF)
      throw Error("unsupported_parser_string_encoding");
    if (!Remaining)
      throw Error("source_model_string_budget_exceeded");
    --Remaining;
    Result.push_back(char16_t(Unit));
  }
  return Result;
}

class Collector {
  SourceAnalysis &Result;
  const char *Base;
  size_t Size;
  uint64_t RemainingUnits = MaxJavaScriptStringUnits;

  void field(uint32_t Parent, const char *Name, AST::NodePtr Child, bool Strict,
             unsigned Depth) {
    if (Child) {
      const auto Index = collect(Child, Parent, Strict, Depth + 1);
      Result.Nodes[Parent].Children.push_back({Name, Index, 0});
    }
  }
  void field(uint32_t Parent, const char *Name, AST::NodeList &Children,
             bool Strict, unsigned Depth) {
    uint32_t Ordinal = 0;
    for (auto &Child : Children) {
      const auto Index = collect(&Child, Parent, Strict, Depth + 1);
      Result.Nodes[Parent].Children.push_back({Name, Index, Ordinal++});
    }
  }
  void field(uint32_t Parent, const char *Name, AST::NodeLabel Value, bool,
             unsigned) {
    if (Value)
      Result.Nodes[Parent].Attributes.push_back(
          {Name, codeUnits(Value->str(), RemainingUnits)});
  }
  void field(uint32_t Parent, const char *Name, bool Value, bool, unsigned) {
    Result.Nodes[Parent].Attributes.push_back({Name, Value});
  }
  void field(uint32_t Parent, const char *Name, double Value, bool, unsigned) {
    Result.Nodes[Parent].Attributes.push_back({Name, Value});
  }

  void fields(AST::Node *Node, uint32_t Index, bool Strict, unsigned Depth) {
    // Read named fields from the pinned parser's own node declarations. This
    // preserves list ordinals and field roles, including shorthand key/value
    // occurrences that cannot be reconstructed from byte spans alone.
#define WEB_FIELDS_0()
#define WEB_FIELDS_1(T, N, O) field(Index, #N, Typed->_##N, Strict, Depth);
#define WEB_FIELDS_2(T, N, O, ...)                                             \
  WEB_FIELDS_1(T, N, O) WEB_FIELDS_1(__VA_ARGS__)
#define WEB_FIELDS_3(T, N, O, ...)                                             \
  WEB_FIELDS_1(T, N, O) WEB_FIELDS_2(__VA_ARGS__)
#define WEB_FIELDS_4(T, N, O, ...)                                             \
  WEB_FIELDS_1(T, N, O) WEB_FIELDS_3(__VA_ARGS__)
#define WEB_FIELDS_5(T, N, O, ...)                                             \
  WEB_FIELDS_1(T, N, O) WEB_FIELDS_4(__VA_ARGS__)
#define WEB_FIELDS_6(T, N, O, ...)                                             \
  WEB_FIELDS_1(T, N, O) WEB_FIELDS_5(__VA_ARGS__)
#define WEB_FIELDS_7(T, N, O, ...)                                             \
  WEB_FIELDS_1(T, N, O) WEB_FIELDS_6(__VA_ARGS__)
#define WEB_FIELDS_8(T, N, O, ...)                                             \
  WEB_FIELDS_1(T, N, O) WEB_FIELDS_7(__VA_ARGS__)
#define WEB_FIELDS_9(T, N, O, ...)                                             \
  WEB_FIELDS_1(T, N, O) WEB_FIELDS_8(__VA_ARGS__)
#define WEB_NODE(COUNT, NAME, BASE, ...)                                       \
  case AST::NodeKind::NAME: {                                                  \
    auto *Typed = llvh::cast<AST::NAME##Node>(Node);                           \
    WEB_FIELDS_##COUNT(__VA_ARGS__) break;                                     \
  }
#define ESTREE_NODE_0_ARGS(NAME, BASE) WEB_NODE(0, NAME, BASE)
#define ESTREE_NODE_1_ARGS(NAME, BASE, ...) WEB_NODE(1, NAME, BASE, __VA_ARGS__)
#define ESTREE_NODE_2_ARGS(NAME, BASE, ...) WEB_NODE(2, NAME, BASE, __VA_ARGS__)
#define ESTREE_NODE_3_ARGS(NAME, BASE, ...) WEB_NODE(3, NAME, BASE, __VA_ARGS__)
#define ESTREE_NODE_4_ARGS(NAME, BASE, ...) WEB_NODE(4, NAME, BASE, __VA_ARGS__)
#define ESTREE_NODE_5_ARGS(NAME, BASE, ...) WEB_NODE(5, NAME, BASE, __VA_ARGS__)
#define ESTREE_NODE_6_ARGS(NAME, BASE, ...) WEB_NODE(6, NAME, BASE, __VA_ARGS__)
#define ESTREE_NODE_7_ARGS(NAME, BASE, ...) WEB_NODE(7, NAME, BASE, __VA_ARGS__)
#define ESTREE_NODE_8_ARGS(NAME, BASE, ...) WEB_NODE(8, NAME, BASE, __VA_ARGS__)
#define ESTREE_NODE_9_ARGS(NAME, BASE, ...) WEB_NODE(9, NAME, BASE, __VA_ARGS__)
    switch (Node->getKind()) {
#include "hermes/AST/ESTree.def"
    default:
      throw Error("unsupported_parser_node");
    }
#undef WEB_NODE
#undef WEB_FIELDS_0
#undef WEB_FIELDS_1
#undef WEB_FIELDS_2
#undef WEB_FIELDS_3
#undef WEB_FIELDS_4
#undef WEB_FIELDS_5
#undef WEB_FIELDS_6
#undef WEB_FIELDS_7
#undef WEB_FIELDS_8
#undef WEB_FIELDS_9
  }

public:
  Collector(SourceAnalysis &Result, const char *Base, size_t Size)
      : Result(Result), Base(Base), Size(Size) {}

  uint32_t collect(AST::Node *Node, uint32_t Parent = UINT32_MAX,
                   bool Strict = false, unsigned Depth = 0) {
    if (Result.Nodes.size() >= MaxJavaScriptNodes || Depth >= 256)
      throw Error("syntax_inventory_budget_exceeded");
    if (Result.SourceType != "module" &&
        (llvh::isa<AST::ImportDeclarationNode>(Node) ||
         llvh::isa<AST::ExportNamedDeclarationNode>(Node) ||
         llvh::isa<AST::ExportDefaultDeclarationNode>(Node) ||
         llvh::isa<AST::ExportAllDeclarationNode>(Node)))
      throw Error("module_syntax_in_script");
    if (auto *Function = llvh::dyn_cast<AST::FunctionLikeNode>(Node))
      Strict = Function->strictness == AST::Strictness::StrictMode;
    if (Strict && llvh::isa<AST::WithStatementNode>(Node))
      throw Error("strict_with_statement");
    const auto Begin = reinterpret_cast<uintptr_t>(Base);
    const auto Start =
        reinterpret_cast<uintptr_t>(Node->getStartLoc().getPointer());
    const auto End =
        reinterpret_cast<uintptr_t>(Node->getEndLoc().getPointer());
    if (Start < Begin || End < Start || End - Begin > Size)
      throw Error("invalid_parser_span");
    SyntaxNode Item;
    Item.Kind = Node->getNodeName().str();
    Item.Start = Start - Begin;
    Item.End = End - Begin;
    Item.Strict = Strict;
    const uint32_t Index = uint32_t(Result.Nodes.size());
    if (Parent != UINT32_MAX)
      Item.ParentID = Result.Nodes[Parent].ID;
    const auto Offset = std::to_string(Item.Start);
    const auto Length = std::to_string(Item.End - Item.Start);
    const auto Ordinal = std::to_string(Index);
    Item.ID = identity("syntax-node",
                       {Result.ID, Item.Kind, Offset, Length, Ordinal});
    Result.Nodes.push_back(std::move(Item));
    fields(Node, Index, Strict, Depth);
    return Index;
  }
};
} // namespace hermes_model
} // namespace neverd::web
