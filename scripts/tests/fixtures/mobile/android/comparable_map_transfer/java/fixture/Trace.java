package fixture;
import java.util.Map;
public final class Trace {
    public static long events;
    public static int actions,comparisons,reads,failAt;
    public static Map input,result;
    public static boolean reverse;
    public static Object payload;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int stage) {events=events*10+stage;if(stage==failAt)throw failure;}
}
