package fixture;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static Holder holder;
    public static String initialValue, afterValue, firstValue, readValue;
    public static String[] first, alias;
    public static int[] second;
    public static int number;
    public static Target seen;
    public static void step(int code) {
        events = events * 17 + code;
        if (code == failAt) throw failure;
    }
    public static String read(Holder input) {
        step(3);
        return readValue = input.value;
    }
    public static int number(int input) { step(4); return input + 1; }
}
