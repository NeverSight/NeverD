package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static LinkedHashMap direct(Collection input,Comparator comparator) {return Factory.make(input,comparator);}
    public static Object ignored(Collection input,Comparator comparator) {Factory.make(input,comparator);return Trace.marker;}
    public static <K,V> LinkedHashMap<K,V> relay(Collection<Map.Entry<K,V>> input,Comparator<Map.Entry<K,V>> comparator) {return Factory.make(input,comparator);}
    public static <K,V> LinkedHashMap<K,V> fromMap(Map<K,V> input,Comparator<Map.Entry<K,V>> comparator) {return Factory.make(input.entrySet(),comparator);}
}
