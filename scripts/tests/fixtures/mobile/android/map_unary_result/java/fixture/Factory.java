package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <K,V> List<Map<K,V>> make(Map<K,? extends Iterable<V>> input) {
        Trace.calls++;Trace.input=input;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (List<Map<K,V>>)Trace.result;
    }
}
