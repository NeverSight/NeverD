package fixture;
import java.util.concurrent.atomic.AtomicReference;
public final class Trace {
    private Trace() {}
    public static int calls,phase;
    public static AtomicReference seen;
    public static Runnable before,after;
    public static void hit(int stage,AtomicReference value) {
        calls++;
        phase=stage;
        seen=value;
        Runnable action=stage==1?before:after;
        if(action!=null) action.run();
    }
}
