package fixture;

import java.util.HashMap;
import java.util.Map;

public class Core<K, V> extends Parent<K, V> {
    public Core(long tag) {
        super(tag, new Store<Wrap<K>, Pair<K, V>>());
    }

    public Core(int unused, long tag) {
        super(tag, new HashMap<Wrap<K>, Pair<K, V>>());
    }

    public Core(long tag, Map<Wrap<K>, Pair<K, V>> map) {
        super(tag, map);
    }
}
