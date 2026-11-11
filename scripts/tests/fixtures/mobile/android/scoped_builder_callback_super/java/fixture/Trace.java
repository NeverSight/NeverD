package fixture;
import java.util.Map;
public class Trace {
    public static int builds,buildActions,views,adapters,allocations,parents,actions,calls,callbackActions,children,wrong,failAt;
    public static long events;
    public static Map<?,?> mapResult,futureResult,selected;
    public static Builder<?,?> builder;
    public static Adapter adapter;
    public static Parent<?,?> parent;
    public static Object input,callbackInput,callbackResult,buildPayload,allocationPayload,parentPayload,callbackPayload;
    public static Runnable buildAction,allocateAction,parentAction,callbackAction;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
