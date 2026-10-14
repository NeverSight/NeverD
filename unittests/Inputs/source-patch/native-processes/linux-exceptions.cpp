
extern "C" int cleanup_count;
extern "C" int original_thrower(int);
struct Guard { ~Guard() { ++cleanup_count; } };
__attribute__((noinline)) int broken() noexcept { throw 31; }
extern "C" int replacement(int x) {
  if (x == 5) return broken();
  if (x == 0) return 40;
  if (x == 1) { try { throw 7; } catch (int value) { return value + 3; } }
  if (x == 2) { Guard g; throw 11; }
  if (x == 3) { Guard g; return original_thrower(17); }
  if (x == 6) {
    int first = 0;
    try { Guard g; try { throw 13; } catch (int value) { first = value; throw 17; } }
    catch (int value) { return first + value + cleanup_count * 10; }
  }
  if (x == 7) {
    Guard outer;
    try { Guard inner; return original_thrower(23); }
    catch (int value) { return value + cleanup_count * 10; }
  }
  try { throw 21; } catch (...) { Guard g; throw; }
}
