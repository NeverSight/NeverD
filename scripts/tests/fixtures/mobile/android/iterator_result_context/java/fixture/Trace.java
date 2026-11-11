package fixture;
import java.util.Iterator;
public final class Trace {
    private Trace() {}
    public static Iterator cursor;
    public static String context;
    public static Object result,marker;
    public static Runnable action;
    public static int calls,stage;
}
