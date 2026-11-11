package fixture;
public final class Trace {
    public static long events;
    public static int failAt,calls,result;
    public static Object receiver,other;
    public static RuntimeException failure;
    public static void hit(int step) { events=events*10+step;if(step==failAt)throw failure; }
}
