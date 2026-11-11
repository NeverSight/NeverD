package fixture;
import java.util.Map;
public class Client {
    public static <K,V> Map<K,V> defaults(Class<K> keys,Class<V> values,Object value) {return Provider.convert(keys,values,value);}
    public static <K,V> Map<K,V> explicit(Class<? extends Map> kind,Class<K> keys,Class<V> values,Object value) {return Provider.convert(kind,keys,values,value);}
}
