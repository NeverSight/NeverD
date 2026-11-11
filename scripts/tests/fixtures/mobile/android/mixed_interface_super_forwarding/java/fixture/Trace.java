package fixture;
public final class Trace {
    public static long events, parameter, observed, result;
    public static Object parent, contextOwner, baseOwner, inputOwner;
    public static Context context;
    public static Input input;
    public static int baseValue;
    public static Runnable beginAction, contextAction, baseAction, inputAction, afterAction;
    public static void hit(int event) { events = events * 10 + event; }
    public static void begin(Object owner, Context ctx, Input arg, long value) {
        parent = owner;
        context = ctx;
        input = arg;
        parameter = value;
        hit(1);
        if (beginAction != null) beginAction.run();
    }
    public static void after(long value) {
        result = value;
        hit(5);
        if (afterAction != null) afterAction.run();
    }
}
