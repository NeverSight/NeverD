package fixture;
import java.io.IOException;
public final class Trace {
    public static long events;
    public static int overrides;
    public static Object receiver,token,value,produced,result;
    public static IOException checked;
    public static Runnable action,providerAfter,after;
    public static void hit(int value) { events=events*16+value; }
}
