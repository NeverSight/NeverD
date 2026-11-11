package fixture;
import java.util.Map;
import java.util.function.Function;
public class Client {
    public static <K,V> V call(Map<K,V> values,K key,Function<? super K,? extends V> function){return Provider.forward(values,key,function);}
    public static <K,V> V full(Map<K,V> values,K key,Function<? super K,? extends V> function){return Provider.adapt(values,key,function);}
}
