//===- ResolverBlockMemo.h - Stable resolver query memo -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"

#include <array>
#include <cstddef>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::detail {

// Keep entries stable while recursive queries grow the hash index. The index
// stores node pointers, so all integer key values remain available; only the
// internal pointer domain reserves DenseSet's empty and tombstone markers.
// Full keys and global entry counts retain the existing evidence charges.
template <typename Key, typename Value> class ResolverBlockMemo {
  using Node = std::pair<const Key, Value>;
  struct NodeInfo {
    static Node *getEmptyKey() {
      return llvm::DenseMapInfo<Node *>::getEmptyKey();
    }
    static Node *getTombstoneKey() {
      return llvm::DenseMapInfo<Node *>::getTombstoneKey();
    }
    static bool marker(Node *N) {
      return N == getEmptyKey() || N == getTombstoneKey();
    }
    static unsigned getHashValue(const Key &K) { return llvm::hash_value(K); }
    static unsigned getHashValue(Node *N) { return getHashValue(N->first); }
    static bool isEqual(Node *A, Node *B) {
      return A == B || (!marker(A) && !marker(B) && A->first == B->first);
    }
    static bool isEqual(const Key &K, Node *N) {
      return !marker(N) && K == N->first;
    }
    static bool isEqual(Node *N, const Key &K) { return isEqual(K, N); }
  };
  using Map = llvm::DenseSet<Node *, NodeInfo>;
  static constexpr size_t Shards = 64;
  std::array<Map, Shards> Maps;
  std::array<bool, Shards> Used{};
  std::vector<size_t> Active;
  size_t Count = 0;

  static size_t shard(const Key &K) {
    return static_cast<unsigned>(std::get<0>(K)) % Shards;
  }

public:
  ResolverBlockMemo() = default;
  ResolverBlockMemo(const ResolverBlockMemo &) = delete;
  ResolverBlockMemo &operator=(const ResolverBlockMemo &) = delete;
  ~ResolverBlockMemo() { clear(); }
  struct Iterator {
    Map *Owner = nullptr;
    Node *Position = nullptr;
    auto *operator->() const { return Position; }
    bool operator==(const Iterator &Other) const {
      return Owner == Other.Owner && (!Owner || Position == Other.Position);
    }
  };
  size_t size() const { return Count; }
  Iterator end() const { return {}; }
  Iterator find(const Key &K) {
    Map &M = Maps[shard(K)];
    const auto I = M.find_as(K);
    return I == M.end() ? end() : Iterator{&M, *I};
  }
  template <typename... Args>
  std::pair<Iterator, bool> try_emplace(const Key &K, Args &&...Values) {
    const size_t S = shard(K);
    Map &M = Maps[S];
    if (const auto I = M.find_as(K); I != M.end())
      return {{&M, *I}, false};
    auto Entry = std::make_unique<Node>(
        std::piecewise_construct, std::forward_as_tuple(K),
        std::forward_as_tuple(std::forward<Args>(Values)...));
    if (!Used[S]) {
      Active.push_back(S);
      Used[S] = true;
    }
    const auto [I, Inserted] = M.insert(Entry.get());
    if (Inserted) {
      Entry.release();
      ++Count;
    }
    return {{&M, *I}, Inserted};
  }
  void erase(Iterator I) {
    I.Owner->erase(I.Position);
    delete I.Position;
    --Count;
  }
  size_t erase(const Key &K) {
    const auto I = find(K);
    if (I == end())
      return 0;
    erase(I);
    return 1;
  }
  void clear() {
    for (size_t S : Active) {
      for (Node *N : Maps[S])
        delete N;
      Maps[S].clear();
      Used[S] = false;
    }
    Active.clear();
    Count = 0;
  }
};

} // namespace neverd::detail
