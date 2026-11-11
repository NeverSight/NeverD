package fixture;
public final class Trace {
    public static long events;
    public static int failAt,calls;
    public static Object seen;
    public static float input;
    public static RuntimeException failure;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
    public static int finish(float value,int bias) { calls++;hit(4);return Float.floatToRawIntBits(value)^bias; }
}
