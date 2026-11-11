package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static RuntimeException failure;
    public static String late;
    public static Context context;
    public static Object parent, child, adapter;
    public static void hit(int event) {
        events=events*17+event;
        if (event==fail) throw failure;
    }
}
