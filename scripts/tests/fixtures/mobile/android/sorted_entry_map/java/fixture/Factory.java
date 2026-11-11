package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <K,V> LinkedHashMap<K,V> make(Collection<Map.Entry<K,V>> input,Comparator<Map.Entry<K,V>> comparator) {
        Trace.calls++;Trace.input=input;Trace.comparator=comparator;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (LinkedHashMap<K,V>)Trace.result;
    }
}
