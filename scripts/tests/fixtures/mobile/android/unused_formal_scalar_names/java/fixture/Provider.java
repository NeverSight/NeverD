package fixture;
public final class Provider<T> {
    static { Trace.hit(1); Trace.values[0] = 41; }
    public static <T> int relay(int input) {
        Trace.hit(2);
        Trace.seen = input;
        int result = input ^ Trace.mask;
        Trace.hit(3);
        return result;
    }
}
