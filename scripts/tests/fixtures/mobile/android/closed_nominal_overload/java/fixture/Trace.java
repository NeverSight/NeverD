package fixture;
public final class Trace {
    private Trace() {}
    public static long events;
    public static int fail,factoryCalls,unexpected;
    public static Throwable failure;
    public static Object partial,legacy,firstSeen,secondSeen;
    public static Object[] restSeen;
    public static Handler handlerSeen,selected,replacement;
    public static void step(int value) {
        events=events*10+value;
        if(fail==value) {
            if(failure instanceof RuntimeException)throw (RuntimeException)failure;
            throw (Error)failure;
        }
    }
    public static void capture(Object first,Object second,Object[] rest,Handler handler) {
        firstSeen=first;secondSeen=second;restSeen=rest;handlerSeen=handler;
        selected=replacement;step(2);
    }
}
