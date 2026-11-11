package fixture;
import java.util.Comparator;
public class Trace {
    public static int calls,failAt,comparisons;
    public static long events;
    public static Object payload,left,right;
    public static Comparator result;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
