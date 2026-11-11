package fixture;
public final class Trace {
    public static long trace;
    public static long bits;
    public static void hit(int event) { trace = trace * 10 + event; }
}
