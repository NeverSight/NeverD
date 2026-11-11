package fixture;
public class Trace {
    public static long events;
    public static int failAt, observed;
    public static RuntimeException failure;
    public static Holder holder;
    public static Object initial, providerValue, inputToken, readValue;
    public static Core seen;
    public static Object[] values, tails;
    public static int[] numbers;
    public static long[] bits;
    public static boolean[] flags;
    public static void step(int stage) {
        events = events * 17 + stage;
        if (failAt == stage) throw failure;
    }
    public static Object read(Holder source, Object token) {
        step(13);
        inputToken = token;
        readValue = source.value;
        return readValue;
    }
    public static void observe(Core source, int stage) {
        observed |= 1 << stage;
        values[stage] = source.value;
        numbers[stage] = source.number;
        bits[stage] = source.bits;
        flags[stage] = source.flag;
        tails[stage] = source.tail;
        step(stage);
    }
}
