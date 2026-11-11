package fixture;
public class Trace {
    public static long events;
    public static int fail,left,right,late;
    public static boolean defaultSeen;
    public static RuntimeException failure;
    public static void hit(int event) {
        events=events*17+event;
        if (event==fail) throw failure;
    }
}
