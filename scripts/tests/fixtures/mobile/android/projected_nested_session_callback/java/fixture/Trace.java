package fixture;
import java.util.concurrent.atomic.AtomicReference;
import kotlin.coroutines.Continuation;
public final class Trace {
    private Trace() {}
    public static int calls,phase;
    public static AtomicReference seenState;
    public static Continuation seenFrame;
    public static Context context;
    public static Object produced,returned;
    public static Runnable first,last;
    public static void step(int stage,AtomicReference state,Continuation frame) {
        calls++; phase=stage; seenState=state; seenFrame=frame;
        if(stage==1 && first!=null) first.run();
        if(stage==3 && last!=null) last.run();
    }
}
