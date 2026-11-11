package fixture;

import java.io.IOException;
import java.util.List;

public class Trace {
    public static int calls,failAt,first,second;
    public static long events;
    public static boolean flag;
    public static List<?> values;
    public static Object payload,result;
    public static Runnable action;
    public static RuntimeException failure;
    public static IOException checked;
    public static void hit(int event) {
        events=events*10+event;
        if (failAt==event) throw failure;
    }
}
