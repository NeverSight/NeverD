package fixture;
public final class Seed {
    private Seed() {}
    public static Object next;
    public static RuntimeException failure;
    public static boolean fail;
    public static Object make() { Trace.hit(6,null);if(fail)throw failure;return next; }
}
