package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Map direct(Iterable input) {return Factory.make(input);}
    public static HashMap checked(Iterable input) {return (HashMap)Factory.make(input);}
    public static Object ignored(Iterable input) {Factory.make(input);return Trace.marker;}
    public static <K,V> Map<K,List<V>> relay(Iterable<? extends Map<K,V>> input) {return Factory.make(input);}
}
