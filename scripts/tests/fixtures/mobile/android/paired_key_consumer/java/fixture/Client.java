package fixture;
import java.util.Map;
import java.util.Comparator;
import java.util.TreeMap;
public class Client {
    public static <K,V> TreeMap<K,V> call(Map<K,V> source){return Provider.forward(source);}
    public static <K,V> TreeMap<K,V> full(Map<K,V> source,Comparator<? super K> comparator){return Provider.adapt(source,comparator);}
}
