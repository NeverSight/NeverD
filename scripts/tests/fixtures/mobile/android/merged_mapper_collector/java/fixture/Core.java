package fixture;

import java.util.function.Function;
import java.util.stream.Collector;
import java.util.stream.Collectors;

public final class Core {
    private Core() {}

    @SuppressWarnings("unchecked")
    public static Bucket owned(Function first, Function second, java.util.function.BinaryOperator merger) {
        return Factory.fuse(first, second, merger);
    }

    @SuppressWarnings("unchecked")
    public static Pair checked(Function first, Function second, java.util.function.BinaryOperator merger) {
        return (Pair)Factory.<Object, String, Integer>fuse(first, second, merger).value;
    }

    @SuppressWarnings("unchecked")
    public static Object ignored(Function first, Function second, java.util.function.BinaryOperator merger) {
        Factory.fuse(first, second, merger);
        return Trace.result;
    }

    @SuppressWarnings("unchecked")
    public static Collector standard(Function first, Function second, java.util.function.BinaryOperator merger) {
        return Collectors.toMap(first, second, merger);
    }
    public static <E,K,V> Bucket<E,?,Pair<K,V>> relay(
            Function<? super E,? extends K> first,
            Function<? super E,? extends V> second,
            java.util.function.BinaryOperator<V> merger) {
        return Factory.fuse(first,second,merger);
    }
}
