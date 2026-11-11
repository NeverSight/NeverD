package fixture;
import java.util.Map;
public final class Values {
    public static <K,V> V pick(Map<K,? extends V> map,K key) {
        return kotlin.collections.MapsKt.getValue(map,key);
    }
}
