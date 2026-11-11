package fixture;
import java.util.Map;
public class Trace {
    public static int calls,failAt;
    public static long events;
    public static Map<?,?> values;
    public static String first,second,result;
    public static String[] tail;
    public static Object payload;
    public static Runnable action;
    public static Failure failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
