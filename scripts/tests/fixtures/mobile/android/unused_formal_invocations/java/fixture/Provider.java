package fixture;
public final class Provider<T> {
    static { Trace.hit(1); }
    public static <T> Object relay(Object input) {
        Trace.hit(2);
        Trace.seen = input;
        if (input != null) ((int[]) input)[0]++;
        Trace.hit(3);
        return Trace.result;
    }
}
