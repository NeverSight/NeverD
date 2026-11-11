package fixture;
public final class Trace {
    public static int fail, events, reads, calls, intInput;
    public static double doubleInput;
    public static float convertedFloat;
    public static long convertedLong;
    public static Core partial;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 10 + event;
        if (fail == event) throw failure;
    }
    public static int readInt(int value) { intInput = value; reads++; hit(1); return value; }
    public static double readDouble(double value) { doubleInput = value; reads++; hit(1); return value; }
}
