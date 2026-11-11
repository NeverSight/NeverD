package fixture;
public class Trace {
    public static long events;
    public static Object receiver;
    public static Input before;
    public static Object valueReceiver;
    public static Runnable action;
    public static Runnable valueAction;
    public static int answer;
    public static void hit(int digit) { events = events * 10 + digit; }
}
