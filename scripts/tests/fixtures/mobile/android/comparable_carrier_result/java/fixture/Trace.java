package fixture;
import java.util.Collection;
public class Trace {
    public static int events;
    public static Collection<?> input;
    public static Object result;
    public static Runnable action;
    public static void hit(int event) { events=events*10+event; }
}
