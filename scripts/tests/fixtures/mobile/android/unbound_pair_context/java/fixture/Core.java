package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Map direct(Iterator cursor,String left,String right) {return Factory.count(cursor,left,right);}
    public static Map from(Iterable source,String left,String right) {return Factory.from(source,left,right);}
    public static HashMap checked(Iterator cursor,String left,String right) {return (HashMap)Factory.count(cursor,left,right);}
    public static Object ignored(Iterator cursor,String left,String right) {Factory.count(cursor,left,right);return Trace.marker;}
    public static <K,V> Map<K,V> relay(Iterator<?> cursor,String left,String right) {return Factory.count(cursor,left,right);}
}
