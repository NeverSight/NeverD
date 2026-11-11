package fixture;

import java.util.HashMap;
import java.util.Map;

public class Parent<K, V> extends Base<K, V> {
    public final long tag;
    public final Map<Wrap<K>, Pair<K, V>> map;

    public Parent(long tag, Map<Wrap<K>, Pair<K, V>> map) {
        this.tag = tag;
        this.map = map;
        Trace.targets++;
        Trace.stage = 4;
    }

    public Pair<K, V> typed(Object key) {
        return map.get(key);
    }

    @SuppressWarnings("rawtypes")
    public Object raw(Object key) {
        return ((Map) map).get(key);
    }
}
