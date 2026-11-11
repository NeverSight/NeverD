package fixture;
import java.util.Comparator;
public final class Trace {
    private Trace() {}
    public static Object owner,value,marker;
    public static Comparator<?> callback;
    public static Runnable action,wrapperAction,baseAction;
    public static Wrapper wrapper;
    public static Comparator<?> parentCallback;
    public static int wrappers,wrapperStage,parentNumber,comparisons,answer;
    public static int bases,calls,shortCalls,number,stage;
}
