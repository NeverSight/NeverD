package fixture;
import java.util.Map;
public class Box<K,V> {
    public final Map<K,V> map;
    public Box(Map<K,V> map) { this.map=map; }
    @SuppressWarnings("unchecked")
    public static <K,V> Box<K,V> wrap(Map<K,V> input) {
        Trace.hit(1);Trace.input=input;Trace.hit(2);
        return (Box<K,V>)Trace.result;
    }
}
