package fixture;
import java.util.Map;
public class Box<K,V> {
    public final Map<K,V> map;
    public Box(Map<K,V> map) { this.map=map; }
    public static <K,V> Box<K,V> create(boolean flag) { return create(37,flag); }
    @SuppressWarnings("unchecked")
    public static <K,V> Box<K,V> create(int capacity,boolean flag) {
        Trace.hit(1);Trace.capacitySeen=capacity;Trace.flagSeen=flag;Trace.hit(2);
        return (Box<K,V>)Trace.result;
    }
}
