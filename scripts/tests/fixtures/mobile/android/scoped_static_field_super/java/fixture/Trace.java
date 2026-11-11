package fixture;
import java.util.concurrent.ConcurrentMap;
public final class Trace {
    public static int parentCalls,completed,failAt;
    public static long events;
    public static ConcurrentMap<?,?> received,other;
    public static Marker observed;
    public static volatile Marker selected;
    public static Object payload,parentObject;
    public static Runnable action;
    public static Failure failure;
    public static void hit(int stage){events=events*10+stage;if(failAt==stage)throw failure;}
}
