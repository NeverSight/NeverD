package fixture;
public final class Trace {
    public static long events;
    public static Object input;
    public static Object receiver;
    public static Object argument;
    public static Context current;
    public static Target constructed;
    public static boolean flag, keep;
    public static Runnable initializer;
    public static Runnable contextAction;
    public static Runnable constructorAction;
    public static Runnable action;
    public static void hit(int stage) { events = events * 10 + stage; }
    public static Context context() {
        Trace.hit(3);
        contextAction.run();
        return current;
    }
}
