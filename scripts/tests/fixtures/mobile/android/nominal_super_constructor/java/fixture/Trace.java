package fixture;
public final class Trace {
    private Trace() {}
    public static Object parentPartial,corePartial,seen,saved;
    public static long stampSeen,events;
    public static int fail;
    public static Throwable failure;
    public static void step(int value) {
        events=events*10+value;
        if(fail==value){if(failure instanceof RuntimeException)throw (RuntimeException)failure;throw (Error)failure;}
    }
}
