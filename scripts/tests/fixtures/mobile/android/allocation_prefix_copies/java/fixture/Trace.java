package fixture;
public class Trace {
    public static long events;
    public static int allocations,fail;
    public static Throwable failure;
    public static Value first,second;
    public static Core partial;
    public static Object other;
    public static void step(int tag) {
        events=events*10+tag;
        other=new Object();
        if(fail==tag) {
            if(failure instanceof RuntimeException) throw (RuntimeException)failure;
            throw (Error)failure;
        }
    }
}
