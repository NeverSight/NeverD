package fixture;

import java.util.HashMap;
import java.util.Map;

public class Core<K, V> extends Base<K, V> {
    public final long tag;
    public final Map<Wrap<K>, Pair<K, V>> map;

    public Core(long tag) {
        this(tag, new Store<Wrap<K>, Pair<K, V>>());
    }

    public Core(int unused, long tag) {
        this(tag, new HashMap<Wrap<K>, Pair<K, V>>());
    }

    public Core(long tag, Map<Wrap<K>, Pair<K, V>> map) {
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
