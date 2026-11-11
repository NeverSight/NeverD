package fixture;
public final class Trace {
    public static long events, bits, input;
    public static Object parent, receiver;
    public static int answer, other, last, overrides;
    public static Runnable action, readAction, afterAction;
    public static void hit(int event) { events = events * 10 + event; }
    public static void begin(Object owner, long value) {
        hit(1);
        parent = owner;
        bits = value;
        action.run();
    }
    public static void after(int value) {
        last = value;
        hit(4);
        afterAction.run();
        hit(5);
    }
}
