package fixture;
public class Holder {
    public static final int word = 37;
    public static final long wide = 0xfedcba9876543210L;
    static { Trace.hit(1); }
}
