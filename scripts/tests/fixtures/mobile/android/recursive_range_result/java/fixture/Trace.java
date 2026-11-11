package fixture;
public final class Trace {
    public static Comparable seenSeed,low,high;
    public static Range seenRange;
    public static long events;
    public static int first,second,failAt;
    public static RuntimeException failure;
    public static void hit(int point) { events=events*10+point;if(failAt==point)throw failure; }
}
