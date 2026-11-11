package fixture;
import java.util.List;
public class Core {
    public static int create(List values, Object needle) { return Finder.find(values, needle); }
    public static int guarded(List values, Object needle) {
        try { return Finder.find(values, needle); }
        catch (Throwable failure) { Trace.last = failure; return -999; }
    }
    public static int locked(List values, Object needle) {
        synchronized (Trace.lock) { return Finder.find(values, needle); }
    }
}
