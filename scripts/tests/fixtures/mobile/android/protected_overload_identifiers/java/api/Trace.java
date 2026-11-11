package api;
public final class Trace {
    private Trace() {}
    public static long events,bits;
    public static int raw,kind,fail;
    public static Throwable failure;
    public static Object ownerSeen,argSeen,callbackInput,result;
    public static Context context;
    public static void step(int value) {
        events=events*10+value;
        if(fail==value) {
            if(failure instanceof RuntimeException)throw (RuntimeException)failure;
            throw (Error)failure;
        }
    }
    public static void capture(Object owner,Object arg,long value,int scale,int selected) {
        ownerSeen=owner;argSeen=arg;bits=value;raw=scale;kind=selected;step(2);step(3);
    }
}
