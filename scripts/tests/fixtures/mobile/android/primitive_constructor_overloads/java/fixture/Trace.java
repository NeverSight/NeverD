package fixture;
public class Trace {
    public static long events;
    public static int seed, fail;
    public static RuntimeException failure;
    public static Object last;
    public static void hit(int event) {
        events = events * 11 + event;
        if (event == fail) throw failure;
    }
    public static long read(Object input) {
        hit(6);
        return ((long[]) input)[0]++ + seed;
    }
}
