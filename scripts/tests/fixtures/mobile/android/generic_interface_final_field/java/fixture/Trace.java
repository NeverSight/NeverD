package fixture;

public final class Trace {
    private Trace() {}
    public static int log, stage;
    public static Runnable action;
    public static Box first, second, third;
    public static Box reFirst, reSecond, reSelf;
    public static Box inputFirst, nextFirst, nextSecond, nextSelf;
    public static Box inputSecond, selfFirst, selfSecond, selfSelf;
}
