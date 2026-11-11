package fixture;
public final class Trace {
    public static long events, bits;
    public static float scale;
    public static boolean flag, checked;
    public static int failAt, reads, callbacks;
    public static Provider receiver;
    public static Object input, nextInput;
    public static Boolean booleanResult;
    public static Reply reply;
    public static RuntimeException failure;
    public static void hit(int step) { events=events*10+step;if(step==failAt)throw failure; }
}
