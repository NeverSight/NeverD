package fixture;
import java.util.*;
public final class Factory {
    private Factory() {}
    @SuppressWarnings("unchecked")
    public static <E> Map<E,Integer> count(Iterator<E> cursor) {
        Trace.calls++;
        Trace.cursor=cursor;
        Trace.stage=1;
        Trace.action.run();
        Trace.stage=2;
        return (Map<E,Integer>)Trace.result;
    }
    public static <E> Map<E,Integer> from(Iterable<E> source) {
        return count(source==null?null:source.iterator());
    }
}
