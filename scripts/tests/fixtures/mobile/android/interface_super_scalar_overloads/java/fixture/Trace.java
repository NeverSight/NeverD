package fixture;
public final class Trace {
    public static long events;
    public static int failAt,calls;
    public static Object seen;
    public static float input;
    public static RuntimeException failure;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
    public static int finish(float value,int bias) { calls++;hit(4);return Float.floatToRawIntBits(value)^bias; }
    public static int inputInt;
    public static long inputLong;
    public static float finishInt(int value) { calls++;hit(4);return Float.intBitsToFloat(value); }
    public static double finishLong(long value) { calls++;hit(4);return Double.longBitsToDouble(value); }
}
