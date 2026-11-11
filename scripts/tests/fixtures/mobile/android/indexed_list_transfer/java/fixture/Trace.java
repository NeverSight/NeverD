package fixture;
import java.util.List;
public final class Trace {
    private Trace() {}
    public static List input;
    public static Object result,marker,built,inner;
    public static Runnable action;
    public static int calls,index,stage;
}
