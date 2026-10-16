// Original header linkage control; no loader algorithm claim.
#include "header_context.h"
#include "neverd/loader/LinkerThunk.h"
namespace neverd {
struct BinaryImage {};
std::optional<LinkerThunk> linkerThunkAt(const BinaryImage &, uint64_t Address,
                                       InstructionMode Mode) {
  LinkerThunk Result;
  Result.Target = Address ^ static_cast<uint8_t>(Mode);
  return Result;
}
std::optional<LinkerThunk> linkerLongBranchThunkAt(const BinaryImage &Image,
                                                 uint64_t Address,
                                                 InstructionMode Mode) {
  return linkerThunkAt(Image, Address + 4, Mode);
}
}
