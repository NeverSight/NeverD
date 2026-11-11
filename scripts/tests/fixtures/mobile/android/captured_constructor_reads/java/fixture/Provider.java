package fixture;
public class Provider {
    public static int word = Trace.initialWord;
    public static long wide = Trace.initialWide;
    static { Trace.hit(1); }
}
