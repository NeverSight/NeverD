package fixture;
public final class Trace {
    public static long events;
    public static int renderOverrides, applyOverrides;
    public static Owner current;
    public static Object input, inputSeen, receiver;
    public static CharSequence output, textSeen;
    public static boolean flag;
    public static Runnable receiverAction, argumentAction, before, after;
    private Trace() { }
    public static void hit(int value) { events = events * 10 + value; }
}
