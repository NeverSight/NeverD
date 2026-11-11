package fixture;
import java.util.Map;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <K,V> Map<K,V> make(K[] keys,V[] values,boolean flag) {
        Trace.calls++;Trace.keys=keys;Trace.values=values;Trace.flag=flag;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (Map<K,V>)Trace.result;
    }
}
