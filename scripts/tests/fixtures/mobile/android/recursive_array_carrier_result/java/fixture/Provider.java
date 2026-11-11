package fixture;
import java.util.List;
class Provider {
    @SuppressWarnings("unchecked")
    public static final <T extends Comparable<? super T>> List<T> select(T[] values) {
        Trace.hit(1); Trace.input = values;
        Trace.hit(2); Trace.action.run(); Trace.hit(3);
        return (List<T>)Trace.result;
    }
}
