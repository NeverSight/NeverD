package fixture;
public final class Trace {
    public static long events, wide;
    public static int failAt,overrides;
    public static RuntimeException failure;
    public static Object input,link,result,partial,overrideResult;
    public static void hit(int value) {
        events=events*10+value;
        if(failAt==value)throw failure;
    }
}
