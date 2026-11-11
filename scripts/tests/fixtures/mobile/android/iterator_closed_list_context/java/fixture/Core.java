package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Object direct(Iterator cursor,String key) {return Factory.at(cursor,key);}
    public static Object from(Iterable source,String key) {return Factory.from(source,key);}
    public static String checked(Iterator cursor,String key) {return (String)Factory.at(cursor,key).get(0);}
    public static Object ignored(Iterator cursor,String key) {Factory.at(cursor,key);return Trace.marker;}
    public static <E> List<Object> relay(Iterator<E> cursor,String key) {return Factory.at(cursor,key);}
}
