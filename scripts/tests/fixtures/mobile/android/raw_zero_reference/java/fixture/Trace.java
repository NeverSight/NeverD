package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static Value value,replacement;
    public static Object current;
    public static void hit(int event) { events=events*10+event; if(event==failAt)throw failure; }
}
