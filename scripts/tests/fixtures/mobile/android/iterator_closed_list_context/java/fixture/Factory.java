package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <E> List<Object> at(Iterator<E> cursor,String key) {
        Trace.calls++;Trace.cursor=cursor;Trace.key=key;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (List<Object>)Trace.result;
    }
    public static <E> List<Object> from(Iterable<E> source,String key) {
        return at(source==null?null:source.iterator(),key);
    }
}
