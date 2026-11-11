package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <E> E at(Iterator<E> cursor,int offset) throws IndexOutOfBoundsException {
        Trace.calls++;Trace.cursor=cursor;Trace.offset=offset;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (E)Trace.result;
    }
    public static <E> E from(Iterable<E> source,int offset) {
        return at(source==null?null:source.iterator(),offset);
    }
}
