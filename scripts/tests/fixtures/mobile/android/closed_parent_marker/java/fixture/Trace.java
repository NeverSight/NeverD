package fixture;
public final class Trace {
    public static int events,parentCount,failAt;
    public static Object parent,core,callbackInput;
    public static Unit unit;
    public static boolean done;
    public static RuntimeException failure;
    public static void hit(int stage) { events=events*10+stage;if(failAt==stage)throw failure; }
}
