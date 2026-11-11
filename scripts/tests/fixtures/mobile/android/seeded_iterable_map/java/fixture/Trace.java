package fixture;
import java.util.*;
public final class Trace {
    private Trace() {}
    public static Iterable input;
    public static Map target;
    public static Mapper key,value;
    public static Object result,marker;
    public static Runnable action;
    public static int calls,stage;
}
