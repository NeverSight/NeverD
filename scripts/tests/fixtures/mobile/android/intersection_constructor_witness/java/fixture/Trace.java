package fixture;
public class Trace {
    public static long events;
    public static Product product;
    public static Bound value;
    public static Flag flag;
    public static int observed;
    public static Runnable initAction, touchAction, action;
    public static void hit(int stage) { events = events * 10 + stage; }
    public static Product finish(Product product) { Trace.hit(8); return product; }
}
