package fixture;

import java.util.function.Function;

public final class Factory {
    private Factory() {}

    @SuppressWarnings("unchecked")
    public static <T, K, V> Bucket<T, ?, Pair<K, V>> fuse(
            Function<? super T, ? extends K> first,
            Function<? super T, ? extends V> second) {
        Trace.targets++;
        Trace.first = first;
        Trace.second = second;
        Object value = Trace.source;
        Trace.seen = value;
        Trace.stage = 1;
        if (Trace.invoke) {
            Trace.left = first.apply((T)value);
            Trace.stage = 2;
            Trace.right = second.apply((T)value);
            Trace.stage = 3;
        }
        Trace.action.run();
        Trace.stage = 4;
        return (Bucket<T, ?, Pair<K, V>>)Trace.result;
    }
}
