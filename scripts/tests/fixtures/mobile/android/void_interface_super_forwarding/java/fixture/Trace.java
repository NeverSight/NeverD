package fixture;
public class Trace {
    public static long events, seed, delta, before, observed;
    public static Object parent, baseOwner, completion;
    public static Runnable beginAction, baseAction, afterAction;
    public static void hit(int digit) { events = events * 10 + digit; }
    public static void begin(Api owner) {
        parent = owner;
        hit(1);
        if (beginAction != null) beginAction.run();
    }
    public static void after(Api owner) {
        completion = owner;
        hit(3);
        if (afterAction != null) afterAction.run();
    }
}
