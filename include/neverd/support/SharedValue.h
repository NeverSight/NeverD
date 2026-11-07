//===- SharedValue.h - A copy-on-write value --------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A value whose copies share one immutable instance until one of them
/// changes: copying and destroying cost a reference count, and the first
/// change of a shared instance copies it.  Image-wide facts that every run
/// of a session reads, such as the call-register summaries of an image, are
/// held so instead of copied into each run.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_SHAREDVALUE_H
#define NEVERD_SUPPORT_SHAREDVALUE_H

#include <memory>
#include <utility>

namespace neverd {

template <typename T> class SharedValue {
public:
  SharedValue() = default;
  SharedValue(T Value) : Held(std::make_shared<T>(std::move(Value))) {}
  SharedValue &operator=(T Value) {
    Held = std::make_shared<T>(std::move(Value));
    Shared = false;
    return *this;
  }

  /// Share \p Value, which no one changes while it is shared.
  void share(std::shared_ptr<const T> Value) {
    Held = std::const_pointer_cast<T>(std::move(Value));
    Shared = true;
  }
  /// The held value as one to share.
  std::shared_ptr<const T> shared() const {
    if (!Held)
      return std::make_shared<const T>();
    Shared = true;
    return Held;
  }

  const T &get() const { return Held ? *Held : none(); }
  operator const T &() const { return get(); }
  const T *operator->() const { return &get(); }

  /// The value to change: its own copy where another holds it.
  T &mutate() {
    if (!Held)
      Held = std::make_shared<T>();
    else if (Shared || Held.use_count() > 1) {
      Held = std::make_shared<T>(*Held);
      Shared = false;
    }
    return *Held;
  }

  // The reads of the held container.
  auto begin() const { return get().begin(); }
  auto end() const { return get().end(); }
  auto size() const { return get().size(); }
  bool empty() const { return get().empty(); }
  template <typename K> auto find(const K &Key) const {
    return get().find(Key);
  }
  template <typename K> auto count(const K &Key) const {
    return get().count(Key);
  }
  template <typename K> decltype(auto) at(const K &Key) const {
    return get().at(Key);
  }
  template <typename K> bool contains(const K &Key) const {
    return get().count(Key) != 0;
  }

  // The changes of the held container, which make the value its own copy
  // first.  None takes an iterator: one of a shared instance would not be
  // the copy's.
  decltype(auto) operator[](const typename T::key_type &Key) {
    return mutate()[Key];
  }
  template <typename... A> decltype(auto) emplace(A &&...Arguments) {
    return mutate().emplace(std::forward<A>(Arguments)...);
  }
  template <typename... A> decltype(auto) try_emplace(A &&...Arguments) {
    return mutate().try_emplace(std::forward<A>(Arguments)...);
  }
  auto insert(const typename T::value_type &Value) {
    return mutate().insert(Value);
  }
  auto erase(const typename T::key_type &Key) { return mutate().erase(Key); }
  void clear() { mutate().clear(); }

  bool operator==(const SharedValue &Other) const {
    return Held == Other.Held || get() == Other.get();
  }

private:
  static const T &none() {
    static const T None;
    return None;
  }
  std::shared_ptr<T> Held;
  /// Whether another may hold Held, as share() and shared() let it.
  mutable bool Shared = false;
};

} // namespace neverd

#endif // NEVERD_SUPPORT_SHAREDVALUE_H
