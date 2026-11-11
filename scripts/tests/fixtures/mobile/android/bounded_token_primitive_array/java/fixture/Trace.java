package fixture;
import java.nio.ByteOrder;
public class Trace {
    public static int calls,failAt;
    public static long events;
    public static byte[] data;
    public static Class<?> type;
    public static ByteOrder order;
    public static Object payload;
    public static Number result;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
