package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static RuntimeException failure;
    public static Error error;
    public static Object parent;
    public static void hit(int event) {
        events=events*17+event;
        if (event==fail) { if(error!=null)throw error; throw failure; }
    }
}
