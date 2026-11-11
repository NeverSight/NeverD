package fixture;
public final class Trace {
    public static long events;
    public static Object parent, transformOwner;
    public static Input input, readOwner;
    public static Output constructed, result;
    public static int mask, outputCalls;
    public static Runnable beginAction, readAction, transformAction, outputAction, afterAction;
    public static void hit(int event) { events = events * 10 + event; }
    public static void begin(Object owner, Input value) {
        hit(1);
        parent = owner;
        input = value;
        if (beginAction != null) beginAction.run();
    }
    public static void after(Output value) {
        result = value;
        hit(5);
        if (afterAction != null) afterAction.run();
    }
}
