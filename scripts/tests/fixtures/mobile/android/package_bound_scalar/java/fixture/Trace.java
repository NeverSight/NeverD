package fixture;
public final class Trace {
    static Provider current;
    static Provider replacement;
    static Provider receiver;
    public static Bound argument;
    public static int observed;
    public static int finished;
    public static long events;
    public static Runnable inputAction;
    public static Runnable action;
    public static void hit(int stage) { events = events * 10 + stage; }
    public static Bound input(Bound value) {
        Trace.hit(2);
        Trace.current = Trace.replacement;
        Trace.inputAction.run();
        return value;
    }
    public static int finish(int result) { Trace.finished = result; Trace.hit(8); return result; }
}
