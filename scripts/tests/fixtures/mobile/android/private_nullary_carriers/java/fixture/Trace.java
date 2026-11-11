package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static boolean override;
    public static float[] values;
    public static Object seed,partial,allocated,result;
    public static void hit(int event) {
        events=events*10+event;
        if(failAt==event)throw failure;
    }
}
