package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static List direct(Map input) {return Factory.make(input);}
    public static ArrayList checked(Map input) {return (ArrayList)Factory.make(input);}
    public static Object ignored(Map input) {Factory.make(input);return Trace.marker;}
    public static <K,V> List<Map<K,V>> relay(Map<K,? extends Iterable<V>> input) {return Factory.make(input);}
}
