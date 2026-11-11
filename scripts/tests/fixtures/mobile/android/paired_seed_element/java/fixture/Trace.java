package fixture;
import java.util.Map;
import java.util.function.Function;
public class Trace {
    public static int calls,callbackCalls,failAt;
    public static long events;
    public static Map<?,?> values;
    public static Function<?,?> function;
    public static Object key,payload,callbackPayload,callbackResult,result,dynamicFunction;
    public static Runnable action;
    public static Failure failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
