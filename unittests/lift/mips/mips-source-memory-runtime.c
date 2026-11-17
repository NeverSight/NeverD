//===- mips-source-memory-runtime.c - Guest source ABI memory copies ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Freestanding memcpy compiled for the selected pointer and size_t widths.
/// All bytes execute in the guest; the emulator does not intercept the call.
//===----------------------------------------------------------------------===//

void *memcpy(void *Destination, const void *Source, __SIZE_TYPE__ Size) {
  unsigned char *Out = Destination;
  const unsigned char *In = Source;
  for (__SIZE_TYPE__ I = 0; I != Size; ++I)
    Out[I] = In[I];
  return Destination;
}
