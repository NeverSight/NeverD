package fixture;
import java.util.Map;
public class Trace {
    public static int calls,actions,failAt;
    public static long events;
    public static boolean flag;
    public static Object key,value,payload,previousValue,result;
    public static Map.Entry created;
    public static Runnable action;
    public static Failure failure;
    public static void hit(int stage){events=events*10+stage;if(stage==failAt)throw failure;}
}
