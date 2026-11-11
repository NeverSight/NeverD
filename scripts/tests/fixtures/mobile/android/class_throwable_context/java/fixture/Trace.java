package fixture;
import java.util.function.Function;
public class Trace {
    public static long events;
    public static int actions,callbacks,overrides;
    public static boolean present;
    public static Object value,payload,seenFactory;
    public static Box<?> receiver;
    public static String message,seenMessage;
    public static Throwable thrown;
    public static Function<String,RuntimeException> factory;
    public static Runnable action;
    public static void hit(int event){events=events*10+event;}
}
