package fixture;
public final class Trace {
    public static int events, mask, failAt;
    public static Throwable failure;
    public static Base seenThis;
    public static Object seenKey, seenLeft, seenRight, seenFinal;
    public static boolean wrong;
    public static void step(int event) {
        events = events * 10 + event;
        if (event == failAt) {
            if (failure instanceof RuntimeException) throw (RuntimeException) failure;
            throw (Error) failure;
        }
    }
}
