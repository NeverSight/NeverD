package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static Throwable failure;
    public static Object partial, seen;
    public static void step(int tag) {
        events = events * 10 + tag;
        if (fail == tag) {
            if (failure instanceof RuntimeException) throw (RuntimeException)failure;
            throw (Error)failure;
        }
    }
}
