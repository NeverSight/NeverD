package fixture;
import java.util.Collection;
public final class Trace {
    private Trace() {}
    public static Collection input;
    public static Classifier classifier;
    public static Object result,marker;
    public static Runnable action;
    public static int calls,stage;
}
