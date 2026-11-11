package fixture;
import java.util.List;
public class Core {
    public static void create(List values) { Sorter.sort(values); }
    public static void guarded(List values) {
        try { Sorter.sort(values); } catch (Throwable failure) { Trace.last = failure; }
    }
    public static void locked(List values) {
        synchronized (Trace.lock) { Sorter.sort(values); }
    }
}
