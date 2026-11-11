package fixture;
public class Trace {
    public static long events;
    public static Runnable outerInit, innerInit, innerAction, outerAction, baseAction, after;
    public static Object head, first, second, leafSelf, wrapSelf, baseSelf, coreSelf;
    public static void hit(int stage) { events=events*16+stage; }
}
