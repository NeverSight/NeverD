package fixture;
public final class Trace {
    public static Object seenContext,seenSeed,seenFn,first,second,result;
    public static long trace;
    public static int failAt;
    public static boolean override;
    public static RuntimeException failure;
    public static void hit(int point) { trace=trace*10+point;if(failAt==point)throw failure; }
}
