package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static Object input, forward, reverse, intermediate, returned, result;
    public static void hit(int event) { events=events*10+event; if(event==failAt)throw failure; }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void observe(Callback forward, Callback reverse) {
        hit(3);
        Bound middle=(Bound)forward.apply(input);
        intermediate=middle; hit(4);
        returned=reverse.apply(middle); hit(5);
    }
}
