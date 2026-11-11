package fixture;
import api.Bound;
public final class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static Object receiver,input,touched,result,after;
    public static Bound output;
    public static void hit(int n) { events=events*10+n;if(failAt==n)throw failure; }
}
