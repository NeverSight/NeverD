package fixture;
import java.util.Map;
public class Trace {
    public static int defaults,parents,actions,allocations,wrong,failAt;
    public static long events;
    public static int compares;
    public static java.util.Comparator<?> capturedComparator,dynamicComparator;
    public static Object left,right;
    public static Runnable compareAction;
    public static Map<?,?> created,selected;
    public static Parent<?,?> parent;
    public static Object payload,allocationPayload,key;
    public static Runnable allocateAction,parentAction;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
