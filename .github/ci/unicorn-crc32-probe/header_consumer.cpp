// Original consumer completes the actual model's struct tag before the header.
#include "header_context.h"
namespace neverd { struct BinaryImage {}; }
#include "neverd/loader/LinkerThunk.h"
int main() {
  neverd::BinaryImage Image;
  const auto Mode = static_cast<neverd::InstructionMode>(1);
  const auto First = neverd::linkerThunkAt(Image, 0x1234, Mode);
  const auto Second = neverd::linkerLongBranchThunkAt(Image, 0x1234, Mode);
  return !First || !Second || First->Target != 0x1235 || Second->Target != 0x1239;
}
