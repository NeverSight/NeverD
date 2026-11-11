package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <K,V> Map<K,V> count(Iterator<V> cursor,String context) {
        Trace.calls++;Trace.cursor=cursor;Trace.context=context;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (Map<K,V>)Trace.result;
    }
    public static <K,V> Map<K,V> from(Iterable<V> source,String context) {
        return count(source==null?null:source.iterator(),context);
    }
}
