package fixture;
import java.util.Collection;
import java.util.function.Function;
public final class Trace {
    private Trace() {}
    public static Collection values;
    public static Function first,second;
    public static Object source,seen,left,right,result,marker;
    public static boolean invoke,ordered;
    public static int targets,stage;
    public static Runnable action;
}
