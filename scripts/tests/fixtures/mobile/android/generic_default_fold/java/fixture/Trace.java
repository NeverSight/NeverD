package fixture;
public final class Trace {
    public static int events, failAt;
    public static Object beforeReceiver, originalSeed, seed, lateSeed, produced, seenResult;
    public static RuntimeException failure;
    public static Error error;
    public static void step(int event) {
        events = events * 10 + event;
        if (failAt == event) throw failure;
        if (failAt == -event) throw error;
    }
    public static void before(Object self, Object input) {
        beforeReceiver = self; originalSeed = input; seed = lateSeed;
        step(1);
    }
    public static void after(Object result) {
        seenResult = result;
        step(3);
    }
}
