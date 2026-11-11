package fixture;
public class Owner {
    public static boolean zeroZ;
    public static byte zeroB;
    public static char zeroC;
    public static short zeroS;
    public static int zeroI;
    public static long zeroJ;
    public static float zeroF;
    public static double zeroD;
    public static Object zeroRef;
    public static String zeroText;
    public static int[] zeroArray;
    public static java.util.List<String> zeroList;
    public static final Object anchor;
    static {
        Trace.hit(1);
        Observer.observe();
        anchor = Trace.seed;
        Trace.hit(2);
        Observer.observe();
        if (Trace.failAt == 5) throw Trace.failure;
        Trace.hit(3);
    }
}
