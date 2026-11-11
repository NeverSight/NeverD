package fixture;
import java.util.*;
@SuppressWarnings("unchecked")
public final class Core {
    private Core() {}
    public static Map direct(Iterator cursor) {return Factory.count(cursor);}
    public static Map from(Iterable source) {return Factory.from(source);}
    public static HashMap checked(Iterator cursor) {return (HashMap)Factory.count(cursor);}
    public static Object ignored(Iterator cursor) {Factory.count(cursor);return Trace.marker;}
    public static <E> Map<E,Integer> relay(Iterator<E> cursor) {return Factory.count(cursor);}
}
