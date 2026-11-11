package fixture;
import java.util.Collection;
public final class Trace {
    private Trace() {}
    public static Collection input,target;
    public static Matcher matcher;
    public static Object result,marker;
    public static Runnable action;
    public static int calls,stage;
}
