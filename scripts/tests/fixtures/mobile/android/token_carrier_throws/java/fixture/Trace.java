package fixture;

import java.io.IOException;
import java.nio.ByteBuffer;

public class Trace {
    public static int calls,failAt;
    public static long events;
    public static ByteBuffer context;
    public static Class<?> token;
    public static Object payload,result;
    public static Runnable action,after;
    public static RuntimeException failure;
    public static IOException checked;
    public static void hit(int event) {
        events=events*10+event;
        if (failAt==event) throw failure;
        if (event==2) after.run();
    }
}
