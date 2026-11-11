package fixture;
import java.util.Map;
import java.util.Comparator;
public class Trace {
    public static int calls,comparisons,actions,order,failAt;
    public static long events;
    public static Map source;
    public static Comparator comparator;
    public static Object first,second,inserted,payload,comparePayload,result;
    public static Runnable action;
    public static Failure failure;
    public static void hit(int stage){events=events*10+stage;if(stage==failAt)throw failure;}
}
