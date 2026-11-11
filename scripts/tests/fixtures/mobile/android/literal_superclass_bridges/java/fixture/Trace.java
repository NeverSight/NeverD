package fixture;
public class Trace {
    public static long events, number;
    public static int seed, fail;
    public static Object last;
    public static RuntimeException failure;
    public static void hit(int event) {
        events = events * 11 + event;
        if (fail == event) throw failure;
    }
    public static void consume(Object note) {
        int[] values = (int[]) note;
        values[0] += seed;
    }
}
