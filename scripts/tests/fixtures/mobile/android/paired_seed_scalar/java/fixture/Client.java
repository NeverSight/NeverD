package fixture;
import java.util.Map;
public class Client {
    public static <K,V> Map.Entry<K,V> call(K key,V value){return Provider.forward(key,value);}
    public static <K,V> Map.Entry<K,V> full(K key,V value,boolean immutable){return Provider.make(key,value,immutable);}
}
