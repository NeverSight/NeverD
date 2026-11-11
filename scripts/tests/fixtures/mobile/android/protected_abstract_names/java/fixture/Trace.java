package fixture;
public class Trace {
    public static int constructed,failAt;
    public static long events,bits;
    public static Object receiver,scope,touched;
    public static boolean output,result,published;
    public static RuntimeException failure;
    public static void hit(int n) { events=events*10+n;if(failAt==n)throw failure; }
}
