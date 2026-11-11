package fixture;

import java.util.function.Function;
import java.util.stream.Collector;
import java.util.stream.Collectors;

public final class Core {
    private Core() {}

    @SuppressWarnings("unchecked")
    public static Bucket owned(Function first, Function second) {
        return Factory.fuse(first, second);
    }

    @SuppressWarnings("unchecked")
    public static Pair checked(Function first, Function second) {
        return (Pair)Factory.<Object, String, Integer>fuse(first, second).value;
    }

    @SuppressWarnings("unchecked")
    public static Object ignored(Function first, Function second) {
        Factory.fuse(first, second);
        return Trace.result;
    }

    @SuppressWarnings("unchecked")
    public static Collector standard(Function first, Function second) {
        return Collectors.toMap(first, second);
    }
}
