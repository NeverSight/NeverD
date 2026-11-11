package fixture;
public final class Trace {
    private Trace() {}
    public static Object partial;
    public static Shape imageSeen;
    public static long offsetSeen,sizeSeen,packed,offsetValue,result,events;
    public static int width,height,fail;
    public static Throwable failure;
    public static void step(int value) {
        events=events*10+value;
        if(fail==value) {if(failure instanceof RuntimeException)throw (RuntimeException)failure;throw (Error)failure;}
        if(value==5)result^=0x100000001L;
    }
    public static long offset() {step(1);return offsetValue;}
    public static long record(long value) {packed=value;step(4);return result;}
}
