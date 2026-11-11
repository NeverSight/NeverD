package fixture;
import java.util.*;
public final class Trace {
    private Trace() {}
    public static Collection input;
    public static Comparator comparator;
    public static Object result,marker,selected,built;
    public static Runnable action;
    public static int calls,stage;
}
