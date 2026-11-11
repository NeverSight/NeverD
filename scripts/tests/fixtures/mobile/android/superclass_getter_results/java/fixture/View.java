package fixture;
import java.util.*;

public final class View<A,B> extends Adapter<A,B> {
    public Set<A> value;
    public View() {}
    public Set<A> getTypedKeys() {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.failure != null) throw Trace.failure;
        return value;
    }
    public Set<Map.Entry<A,B>> entrySet() { return null; }
    public static Set<?> select(View<?,?> value) { return value.getTypedKeys(); }
    public Set<Object> originalSuperKeys() { return super.getKeys(); }
}
