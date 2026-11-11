package fixture;
public final class Trace {
    public static int enters, callbacks, virtuals, events, fail;
    public static Object receiver, predicate;
    public static RuntimeException failure;
    public static void enter(Object self, Object callback) {
        enters++; receiver = self; events = events * 10 + 1;
        if (fail == 1) throw failure;
        predicate = callback; events = events * 10 + 2;
        if (fail == 2) throw failure;
    }
}
