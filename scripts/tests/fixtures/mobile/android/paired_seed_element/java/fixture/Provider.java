package fixture;
import java.util.Map;
import java.util.function.Function;
public class Provider {
    public static <K,V> V forward(Map<K,V> values,K key,Function<? super K,? extends V> function){return adapt(values,key,function);}
    @SuppressWarnings("unchecked")
    public static <K,V> V adapt(Map<K,V> values,K key,Function<? super K,? extends V> function){Trace.calls++;Trace.values=values;Trace.key=key;Trace.function=function;Trace.hit(1);Trace.action.run();Trace.hit(2);V computed=function.apply(key);Trace.callbackResult=computed;Trace.hit(3);values.put(key,computed);Trace.hit(4);return (V)Trace.result;}
}
