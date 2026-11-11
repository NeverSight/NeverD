package fixture;
public final class Trace {
    public static String events="";
    public static int failAt;
    public static int virtualCalls;
    public static Source source;
    public static State state;
    public static RuntimeException failure;
    public static void hit(int event) {
        events+=event+",";
        if(failAt==event)throw failure;
    }
}
