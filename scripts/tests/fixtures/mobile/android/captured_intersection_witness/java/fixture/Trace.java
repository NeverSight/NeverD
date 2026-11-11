package fixture;
public final class Trace {
    public static long events;
    public static int indexSeen;
    public static Object context, contextSeen, captured, receiver, argument;
    public static String[] inputs;
    public static Runnable contextAction, construct, before, action;
    public static void hit(int event) { events = events * 10 + event; }
}
