package fixture;
import java.util.Map;
public final class Trace {
    public static int reads,targets,calls,wrong,failAt,nextSize,snapshot;
    public static long events;
    public static float factor;
    public static Map<?,?> readSource,afterSource;
    public static Object factory,created,instance,payload;
    public static Runnable readAction,targetAction;
    public static RuntimeException failure;
    public static void hit(int stage){events=events*10+stage;if(failAt==stage)throw failure;}
}
