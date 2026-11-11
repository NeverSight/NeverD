package fixture;
public final class Trace {
    private Trace() {}
    public static long events,bits;
    public static int fail;
    public static boolean resume;
    public static Throwable failure;
    public static Object sessionSeen,outerSeen,tokenSeen,callbackOwner,completionSeen,resumedValue;
    public static Object resumeValue,result,otherScope;
    public static void step(int value) {
        events=events*10+value;
        if(fail==value) {
            if(failure instanceof RuntimeException)throw (RuntimeException)failure;
            throw (Error)failure;
        }
    }
    public static void captureWindow(Object session,Object outer,Object token,long value) {
        sessionSeen=session;outerSeen=outer;tokenSeen=token;bits=value;step(2);step(3);
    }
}
