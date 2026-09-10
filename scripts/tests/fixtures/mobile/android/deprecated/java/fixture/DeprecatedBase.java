package fixture;

@Deprecated
public class DeprecatedBase {
    @Deprecated
    public int legacy;
    public int current;
    public static int constructorCalls;

    @Deprecated
    public DeprecatedBase() {
        constructorCalls++;
        legacy = 7;
        current = 11;
    }

    public DeprecatedBase(int seed) {
        constructorCalls++;
        legacy = seed;
        current = seed + 3;
    }

    @Deprecated
    public int oldAdd(int delta) {
        int previous = legacy;
        legacy = previous + delta;
        return previous;
    }

    public int combine(int value) {
        return current * 3 - value;
    }
}
