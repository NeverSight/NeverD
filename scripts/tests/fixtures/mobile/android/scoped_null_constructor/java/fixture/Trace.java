package fixture;
import java.util.Comparator;
public final class Trace {
    private Trace() {}
    public static Object owner,value,marker;
    public static Comparator<?> callback;
    public static Runnable action;
    public static int bases,calls,shortCalls,number,stage;
}
