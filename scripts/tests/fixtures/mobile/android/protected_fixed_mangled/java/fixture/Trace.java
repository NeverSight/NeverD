package fixture;
import api.*;
public final class Trace {
    public static int constructed,failAt;
    public static long events,bits;
    public static RuntimeException failure;
    public static Object receiver,scope,sink,touched,accepted,result,output;
    public static Value value;
    public static void hit(int n) { events=events*10+n;if(failAt==n)throw failure; }
}
