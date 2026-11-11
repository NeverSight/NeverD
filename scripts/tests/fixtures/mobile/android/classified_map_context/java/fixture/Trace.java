package fixture;
import java.util.*;
import java.util.function.Function;
public final class Trace {
    private Trace() {}
    public static Collection values;
    public static Function classifier;
    public static Object result,marker;
    public static Runnable action;
    public static boolean flag;
    public static int calls,stage,kind;
}
