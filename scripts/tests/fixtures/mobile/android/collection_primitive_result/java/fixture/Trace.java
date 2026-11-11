package fixture;
import java.util.Collection;
public final class Trace {
    private Trace() {}
    public static Collection source;
    public static Test test;
    public static Object result,marker;
    public static Runnable action;
    public static int calls,stage,index,value;
}
