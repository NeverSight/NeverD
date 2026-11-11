import fixture.Core;
import fixture.Pair;
import fixture.Wrap;
import java.util.Map;

public final class AllocatedNestedCarrierDelegationCaller {
    public static Core<String, Integer> create(int route, long tag,
            Map<Wrap<String>, Pair<String, Integer>> map) {
        switch (route) {
        case 0: return new Core<String, Integer>(tag);
        case 1: return new Core<String, Integer>(tag, map);
        case 2: return new Core<String, Integer>(0, tag);
        default: throw new AssertionError("route");
        }
    }

    public static Object raw(Core<String, Integer> value, Object key) {
        return value.raw(key);
    }

    public static Pair<String, Integer> typed(Core<String, Integer> value, Object key) {
        return value.typed(key);
    }
}
