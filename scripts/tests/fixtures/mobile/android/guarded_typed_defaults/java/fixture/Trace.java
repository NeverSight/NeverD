package fixture;
public final class Trace {
    public static long events,bits;
    public static int fail;
    public static RuntimeException failure;
    public static Observer observer;
    public static Child partial;
    public static Object fallback,reply,seenInput;
    public static void hit(int point) { events=events*31+point;if(point==fail)throw failure; }
    public static long wide() { hit(1);return bits; }
    public static Object choice(Object token) { hit(2);return token; }
}
