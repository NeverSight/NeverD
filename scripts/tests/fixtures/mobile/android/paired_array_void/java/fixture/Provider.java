package fixture;
public class Provider {
    static { Trace.hit(1); }
    public static <K, V> void apply(Sink<? super K, ? super V> sink, Pair<? extends K, ? extends V>[] pairs) {
        Trace.hit(2);
        int count = pairs.length;
        for (int index = 0; index < count; index++) {
            Pair<? extends K, ? extends V> pair = pairs[index];
            K key = pair.component1();
            V value = pair.component2();
            sink.put(key, value);
            Trace.hit(7);
        }
    }
}
