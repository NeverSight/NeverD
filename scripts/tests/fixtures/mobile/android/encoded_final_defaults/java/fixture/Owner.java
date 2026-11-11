package fixture;
public class Owner {
    public static final boolean aZ;
    public static final byte aB;
    public static final char aC;
    public static final short aS;
    public static final int aI;
    public static final long aJ;
    public static final float aF;
    public static final double aD;
    public static final Object aRef;
    public static final Object[] aArray;
    public static final int zzzAnchor = 7;
    static {
        Trace.hit(1);
        Observer.observe(0);
        aZ = Trace.z; aB = Trace.b; aC = Trace.c; aS = Trace.s;
        aI = Trace.i; aJ = Trace.j; aF = Trace.f; aD = Trace.d;
        aRef = Trace.seed; aArray = Trace.array;
        Trace.hit(2);
        Observer.observe(1);
        Trace.hit(3);
    }
}
