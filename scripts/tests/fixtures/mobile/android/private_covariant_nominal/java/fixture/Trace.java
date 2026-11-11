package fixture;
public final class Trace {
    public static long trace;
    public static Provider receiver;
    public static void hit(int event) { trace = trace * 10 + event; }
}
