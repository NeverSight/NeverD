package fixture;
public final class Trace {
    public static String events="";
    public static int failAt;
    public static int virtualCalls;
    public static Source source;
    public static State state;
    public static Scope scope;
    public static Scope calledScope;
    public static Unit callback;
    public static Unit calledUnit;
    public static Unit returnedUnit;
    public static Object result;
    public static RuntimeException failure;
    public static void hit(int event) {
        events+=event+",";
        if(failAt==event)throw failure;
    }
}
