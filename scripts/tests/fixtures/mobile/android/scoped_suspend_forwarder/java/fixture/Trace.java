package fixture;
public final class Trace {
    private Trace() {}
    public static long events,bits;
    public static int fail;
    public static boolean resume;
    public static Throwable failure;
    public static Object ownerSeen,contextSeen,callbackOwner,completionSeen,resumedValue;
    public static Object contextValue,resumeValue,result;
    public static void step(int value) {
        events=events*10+value;
        if(fail==value) {
            if(failure instanceof RuntimeException)throw (RuntimeException)failure;
            throw (Error)failure;
        }
    }
    public static void captureWindow(Object owner,Object context,long value) {
        ownerSeen=owner;contextSeen=context;bits=value;step(2);step(3);
    }
    public static void captureGauge(Object owner,Object context,double value) {
        ownerSeen=owner;contextSeen=context;bits=Double.doubleToRawLongBits(value);step(2);step(3);
    }
}
