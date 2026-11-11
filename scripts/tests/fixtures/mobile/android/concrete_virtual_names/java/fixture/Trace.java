package fixture;
public class Trace {
    public static long events;
    public static int seed, fail;
    public static Object value, initialized;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 11 + event;
        if (event == fail) throw failure;
    }
    public static Object consume(Object input) {
        int[] values = (int[]) input;
        values[0] += seed;
        Object result = value;
        hit(3);
        value = null;
        return result;
    }
    public static void __neverdVirtualMethod0() {}
}
