package fixture;
public final class Trace {
    public static int events, failAt, after, constructors, innerConstructors;
    public static RuntimeException failure;
    public static Holder holder;
    public static int[] prepared, replaced;
    public static Object target, inner;
    public static void hit(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
    }
}
