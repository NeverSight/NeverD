import fixture.*;
public final class AllocationIntegerArithmeticCaller {
    public static Target make(int kind, int x, int y, int delta) {
        return kind == 0 ? Core.make(x, y, delta) : Core.bias(x, y);
    }
}
