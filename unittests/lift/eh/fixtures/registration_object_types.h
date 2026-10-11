// Exact source types for the trivial PE32 object runtime and syntax checks.
#ifndef NEVERD_TEST_REGISTRATION_OBJECT_TYPES_H
#define NEVERD_TEST_REGISTRATION_OBJECT_TYPES_H

struct ValueObject {
  int Head;
  int Tail;
};
struct ReferenceObject {
  unsigned Head;
  unsigned Tail;
  unsigned Tag;
};

#endif
