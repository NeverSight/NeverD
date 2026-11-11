package fixture;
import java.util.List;
public final class Trace {
    public static long events;
    public static int actions,reads,failAt;
    public static List values,result;
    public static Object seed,payload;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int stage){events=events*10+stage;if(stage==failAt)throw failure;}
}
