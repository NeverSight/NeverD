package fixture;
import java.util.Collection;
import java.util.function.Function;
import java.util.stream.Collector;
public final class Trace {
    private Trace() {}
    public static Collection values;
    public static Function classifier;
    public static Collector downstream;
    public static Object result,marker;
    public static Runnable action;
    public static boolean flag;
    public static int calls,stage;
}
