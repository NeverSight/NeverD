//===- JSON.cpp - Read bounded model JSON with unique object keys ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Parse model headers, sidecars and scan options.
//===----------------------------------------------------------------------===//

#include "ModelInternal.h"

#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace neverd::model {
namespace {

/// Build the DOM directly so duplicate detection uses the object's own index.
/// The JSON lexer calls this SAX interface once per event.
class ModelJSONParser {
  Budget &ReadBudget;
  llvm::SmallVector<Json *> Containers;
  // The root and object members already have a slot. Array elements are
  // appended when their value arrives. An open array grows only between
  // children, so pointers to open containers stay valid.
  Json *NextSlot;

  void checkEvent() {
    ReadBudget.tick();
    if (Containers.size() > limits::kDepth)
      throw Error("JSON nesting exceeds limit");
  }

  Json &append(Json Value) {
    checkEvent();
    if (Json *Slot = std::exchange(NextSlot, nullptr)) {
      *Slot = std::move(Value);
      return *Slot;
    }
    return Containers.back()->emplace_back(std::move(Value));
  }

  bool scalar(Json Value) {
    append(std::move(Value));
    return true;
  }

  bool startContainer(Json::value_t Kind) {
    Json &Container = append(Kind);
    Containers.push_back(&Container);
    return true;
  }

  bool endContainer() {
    Containers.pop_back();
    checkEvent();
    return true;
  }

public:
  ModelJSONParser(Json &Root, Budget &ReadBudget)
      : ReadBudget(ReadBudget), NextSlot(&Root) {}

  bool null() { return scalar(nullptr); }
  bool boolean(bool Value) { return scalar(Value); }
  bool number_integer(Json::number_integer_t Value) { return scalar(Value); }
  bool number_unsigned(Json::number_unsigned_t Value) { return scalar(Value); }
  bool number_float(Json::number_float_t Value, const Json::string_t &) {
    return scalar(Value);
  }
  bool string(Json::string_t &Value) { return scalar(std::move(Value)); }
  bool binary(Json::binary_t &Value) { return scalar(std::move(Value)); }
  bool start_object(std::size_t) {
    return startContainer(Json::value_t::object);
  }
  bool end_object() { return endContainer(); }
  bool start_array(std::size_t) { return startContainer(Json::value_t::array); }
  bool end_array() { return endContainer(); }

  bool key(Json::string_t &Key) {
    checkEvent();
    auto &Members = Containers.back()->get_ref<Json::object_t &>();
    auto [It, Inserted] = Members.try_emplace(std::move(Key));
    if (!Inserted)
      throw Error("duplicate JSON object key");
    NextSlot = &It->second;
    return true;
  }

  bool parse_error(std::size_t, const std::string &, const Json::exception &) {
    return false;
  }
};
} // namespace

Json parseObject(std::string_view Data, Budget &B) {
  Json Root;
  ModelJSONParser Parser(Root, B);
  if (!Json::sax_parse(Data, &Parser) || !Root.is_object())
    throw Error("invalid JSON object");
  return Root;
}
} // namespace neverd::model
