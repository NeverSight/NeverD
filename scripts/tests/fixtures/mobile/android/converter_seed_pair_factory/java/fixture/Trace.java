package fixture;
public final class Trace {
    public static int events,failAt,kind;
    public static Object converter,extracted,partial,created,other;
    public static RuntimeException failure;
    public static void hit(int event) { events=events*10+event;if(failAt==event)throw failure; }
}
