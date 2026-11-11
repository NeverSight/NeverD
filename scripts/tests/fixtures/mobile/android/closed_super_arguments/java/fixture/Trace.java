package fixture;
public final class Trace {
    public static long events,bits;
    public static int fail;
    public static RuntimeException failure;
    public static Fn<Object,Unit> first,second;
    public static Parent partial;
    public static Object reply,seenInput,seenCallback;
    public static void hit(int point) { events=events*31+point;if(point==fail)throw failure; }
    public static long wide() { hit(1);return bits; }
    public static Fn<Object,Unit> read() { hit(3);return first; }
    public static Fn<Object,Unit> write() { hit(5);return second; }
}
