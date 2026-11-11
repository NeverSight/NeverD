package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <K,V> Map<K,List<V>> make(Iterable<? extends Map<K,V>> input) {
        Trace.calls++;Trace.input=input;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (Map<K,List<V>>)Trace.result;
    }
}
