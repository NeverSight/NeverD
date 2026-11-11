package fixture;
import java.util.List;
public class Trace {
    public static long events;
    public static int failAt, observedResult, result;
    public static Object lock = new Object(), values, needle;
    public static boolean held;
    public static RuntimeException failure;
    public static Throwable last;
    public static void hit(int event) {
        events = events * 10 + event;
        held = Thread.holdsLock(lock);
        if (event == failAt) throw failure;
    }
    public static void before(List input, Object key) {
        values = input; needle = key; hit(1);
    }
    public static void after(List input, Object key, int value) {
        values = input; needle = key; observedResult = value;
        result = value ^ 0x5a5a; hit(2);
    }
}
