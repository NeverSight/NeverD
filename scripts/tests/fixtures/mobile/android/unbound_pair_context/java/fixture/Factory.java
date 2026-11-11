package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <K,V> Map<K,V> count(Iterator<?> cursor,String left,String right) {
        Trace.calls++;Trace.cursor=cursor;Trace.left=left;Trace.right=right;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (Map<K,V>)Trace.result;
    }
    public static <K,V> Map<K,V> from(Iterable<?> source,String left,String right) {
        return count(source==null?null:source.iterator(),left,right);
    }
}
