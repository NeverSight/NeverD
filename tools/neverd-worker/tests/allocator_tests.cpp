#include "neverd/sdk/NeverDCAPISession.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mimalloc.h>
#include <new>
#include <thread>

int main() {
  if (mi_version() != MI_MALLOC_VERSION)
    return 1;
  // The SDK is a separate DSO. Its C++ allocations must use the same heap as
  // the worker and libc when the executable embeds the override.
  auto session = neverd_session_create();
  if (!session || !mi_is_in_heap_region(session))
    return 2;
  neverd_session_destroy(session);
  for (size_t size :
       {size_t{0}, size_t{16}, size_t{1024}, size_t{4096}, size_t{8192}}) {
    auto *before = static_cast<unsigned char *>(std::malloc(size));
    if (!before || !mi_is_in_heap_region(before))
      return 3;
    std::memset(before, 0x56, size);
    auto *after = static_cast<unsigned char *>(std::realloc(before, size + 7));
    if (!after || !mi_is_in_heap_region(after))
      return 4;
    for (size_t i = 0; i < size; ++i)
      if (after[i] != 0x56)
        return 5;
    std::thread([after] { std::free(after); }).join();
  }
  struct alignas(4096) Aligned {
    char bytes[4096];
  };
  auto *aligned = new Aligned;
  if (reinterpret_cast<size_t>(aligned) % alignof(Aligned) ||
      !mi_is_in_heap_region(aligned))
    return 6;
  std::thread([aligned] { delete aligned; }).join();
  auto *text = ::strdup("libc allocation across a thread boundary");
  if (!text || !mi_is_in_heap_region(text))
    return 7;
  std::thread([text] { std::free(text); }).join();
  std::cout
      << "SDK, libc, realloc, over-alignment and cross-thread release passed\n";
}
