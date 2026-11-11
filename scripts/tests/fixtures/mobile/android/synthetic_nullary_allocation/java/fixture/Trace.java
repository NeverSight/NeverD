package fixture;
import java.util.Map;
public final class Trace {
    public static int adapters,targets,calls,wrong,failAt;
    public static long events;
    public static float factor;
    public static Map<?,?> map;
    public static Object adapter,factory,result,created,instance,payload;
    public static Runnable allocateAction,makeAction,targetAction;
    public static RuntimeException failure;
    public static void hit(int stage){events=events*10+stage;if(failAt==stage)throw failure;}
}
