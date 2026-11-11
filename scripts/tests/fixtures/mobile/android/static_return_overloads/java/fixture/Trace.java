package fixture;
public final class Trace {
 public static int events, mode, slot, flags; public static long wide;
 public static Object input, box, factory, stamp, after, result;
 public static RuntimeException failure;
 public static void hit(int value){events=events*10+value;}
}
