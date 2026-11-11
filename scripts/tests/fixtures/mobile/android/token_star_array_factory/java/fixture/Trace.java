package fixture;
public class Trace {
    public static int events;
    public static Class<?> type;
    public static Class<?>[] parts;
    public static Object result;
    public static Runnable action;
    public static void hit(int event) { events=events*10+event; }
}
