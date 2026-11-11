package fixture;
public final class Trace {
    public static long events;
    public static Parent<?> receiver;
    public static Object seed;
    public static Token<?> token;
    public static float observed;
    public static float weight;
    public static int kind;
    public static Runnable initAction;
    public static Runnable action;
    public static void hit(int stage) { events = events * 10 + stage; }
    public static Product finish(Product product) { Trace.hit(8); return product; }
}
