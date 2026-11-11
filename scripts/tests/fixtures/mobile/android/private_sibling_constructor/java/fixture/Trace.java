package fixture;

import java.io.IOException;

public class Trace {
    public static long events;
    public static int failAt;
    public static RuntimeException failure;
    public static IOException checked;
    public static void hit(int event) {
        events=events*10+event;
        if (failAt==event) throw failure;
    }
}
