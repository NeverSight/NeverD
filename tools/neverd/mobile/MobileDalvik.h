//===- MobileDalvik.h - Typed native Dalvik recovery model
//-----------------===//
#pragma once

#include "MobileCommon.h"

#include <compare>
#include <map>
#include <set>
#include <variant>

namespace neverd::mobile::dalvik {
using Access = std::set<std::string>;
bool has(const Access &access, std::string_view flag);
std::string descriptor(std::string_view value, bool allow_void = false);
std::pair<std::vector<std::string>, std::string>
prototype(std::string_view value);
unsigned width(std::string_view type);
Access accessFlags(uint32_t value);

struct MethodRef {
  std::string owner;
  std::string name;
  std::vector<std::string> parameters;
  std::string returns;
  std::string signature() const;
  std::string identity() const;
  auto operator<=>(const MethodRef &) const = default;
};
struct FieldRef {
  std::string owner;
  std::string name;
  std::string type;
  auto operator<=>(const FieldRef &) const = default;
};
MethodRef methodRef(std::string_view value);
FieldRef fieldRef(std::string_view value);

// String literals contain UTF-8, with isolated UTF-16 surrogates represented
// in WTF-8 until the Java string emitter escapes their exact code units.
using Literal = std::variant<std::monostate, int64_t, std::string>;
using Reference =
    std::variant<std::monostate, MethodRef, FieldRef, std::string>;
struct FloatBits {
  uint64_t bits = 0;
  bool wide = false;
};
using FieldValue =
    std::variant<std::monostate, int64_t, std::string, bool, FloatBits>;

struct Instruction {
  uint32_t pc = 0;
  std::string opcode;
  std::vector<unsigned> registers;
  Literal literal;
  std::optional<uint32_t> target;
  Reference reference;
  std::vector<int32_t> keys;
  std::vector<uint32_t> targets;
  std::vector<uint64_t> data;
  unsigned element_width = 0;
};
struct Handler {
  std::optional<std::string> type;
  uint32_t target = 0;
};
struct TryRegion {
  uint32_t start = 0;
  uint32_t end = 0;
  std::vector<Handler> handlers;
};
struct Method {
  MethodRef reference;
  Access access;
  unsigned registers = 0;
  std::vector<Instruction> instructions;
  std::vector<TryRegion> tries;
  uint32_t code_end = 0;
  unsigned incomingWords() const;
};
struct Field {
  FieldRef reference;
  Access access;
  FieldValue value;
};
struct Class {
  std::string name;
  std::optional<std::string> superclass;
  Access access;
  std::string source_id;
  std::vector<std::string> interfaces;
  std::vector<Field> fields;
  std::vector<Method> methods;
  std::optional<std::string> enclosing;
  std::optional<std::string> inner_name;
  Access inner_access;
};
using ClassMap = std::map<std::string, Class>;
ClassMap linkClasses(std::vector<Class> classes, Budget &budget);
std::vector<Class> parseDex(std::string_view bytes, std::string_view input_id,
                            Budget &budget);
Class parseSmali(std::string_view text, std::string_view input_id,
                 Budget &budget);
llvm::json::Object recoverJava(const ClassMap &classes, Budget &budget);
} // namespace neverd::mobile::dalvik
