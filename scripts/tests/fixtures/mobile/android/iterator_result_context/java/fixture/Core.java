package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Map direct(Iterator cursor,String context) {return Factory.count(cursor,context);}
    public static Map from(Iterable source,String context) {return Factory.from(source,context);}
    public static HashMap checked(Iterator cursor,String context) {return (HashMap)Factory.count(cursor,context);}
    public static Object ignored(Iterator cursor,String context) {Factory.count(cursor,context);return Trace.marker;}
    public static <K,V> Map<K,V> relay(Iterator<V> cursor,String context) {return Factory.count(cursor,context);}
}
