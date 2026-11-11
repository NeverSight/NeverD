package fixture;
import java.util.Map;
public class Factory<K,V> {
    public final Map<K,V> map;
    public Factory(Map<K,V> map) { this.map=map; }
    public static <K,V> Factory<K,V> create(boolean flag) { return create(37,flag); }
    @SuppressWarnings("unchecked")
    public static <K,V> Factory<K,V> create(int capacity,boolean flag) {
        Trace.hit(1);Trace.capacitySeen=capacity;Trace.flagSeen=flag;
        Trace.action.run();Trace.hit(2);return (Factory<K,V>)Trace.result;
    }
}
