package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static int calls;
    public static RuntimeException failure;
    public static Object result;
    public static Object marker;
    public static void event(int event) {
        events = events * 10 + event;
        if (event == fail) throw failure;
    }
    public static Object apply(Object input) {
        calls++;
        event(2);
        if (input != null) ((int[])input)[0]++;
        return input;
    }
}
