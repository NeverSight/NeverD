package fixture;
import java.util.Map;
public final class Trace {
    private Trace() {}
    public static Map input;
    public static Object result,marker,built,inner;
    public static Runnable action;
    public static int calls,stage;
}
