#include "neverd/loader/ObjC/ObjCEncoding.h"

namespace neverd {
// Deliberately small source-projection grammar. It preserves scalar widths
// and signedness, but does not invent aggregate or callable block/Swift ABI
// rules.
TypeRef parseObjCScalarType(llvm::StringRef Encoding, size_t &I,
                            unsigned Depth) {
  if (Depth > 16)
    return nullptr;
  while (I < Encoding.size() &&
         llvm::StringRef("rnNoORV").contains(Encoding[I]))
    ++I;
  if (I >= Encoding.size())
    return nullptr;
  const char C = Encoding[I++];
  switch (C) {
  case 'v':
    return NdType::makeVoid();
  case 'c':
    return NdType::makeInt(1, true);
  case 'C':
  case 'B':
    return NdType::makeInt(1, false);
  case 's':
    return NdType::makeInt(2, true);
  case 'S':
    return NdType::makeInt(2, false);
  case 'i':
    return NdType::makeInt(4, true);
  case 'I':
    return NdType::makeInt(4, false);
  // Darwin uses q/Q for 64-bit long; legacy l/L encodings remain 32 bits.
  case 'l':
    return NdType::makeInt(4, true);
  case 'L':
    return NdType::makeInt(4, false);
  case 'q':
    return NdType::makeInt(8, true);
  case 'Q':
    return NdType::makeInt(8, false);
  case 'f':
    return NdType::makeFloat(4);
  case 'd':
    return NdType::makeFloat(8);
  case '#':
  case ':':
    return NdType::makePtr(NdType::makeVoid());
  case '*':
    return NdType::makePtr(NdType::makeInt(1));
  case '@':
    if (I < Encoding.size() && Encoding[I] == '?') {
      ++I; // This pointer does not describe the block's invoke ABI.
      return NdType::makePtr(NdType::makeVoid());
    }
    if (I < Encoding.size() && Encoding[I] == '"') {
      const auto End = Encoding.find('"', ++I);
      if (End == llvm::StringRef::npos)
        return nullptr;
      I = End + 1;
    }
    return NdType::makePtr(NdType::makeVoid());
  case '^': {
    auto Pointee = parseObjCScalarType(Encoding, I, Depth + 1);
    return Pointee ? NdType::makePtr(Pointee) : nullptr;
  }
  default:
    return nullptr;
  }
}

} // namespace neverd
