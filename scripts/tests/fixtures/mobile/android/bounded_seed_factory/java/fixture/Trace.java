package fixture;
public final class Trace {
    public static Object context,extra,seed,factory,item,observed,resultInput,result,argument;
    public static long bits,observedValue,events;
    public static int failAt,calls;
    public static RuntimeException failure;
    public static void hit(int event) { events=events*10+event;if(event==failAt)throw failure; }
}
