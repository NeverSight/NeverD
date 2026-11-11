package fixture;

public final class Core {
    private Core() {}
    public static java.util.List<Object> raw(Iterable<Object> source, java.util.function.Function<Object,Object> mapper, boolean flag) {
        return Provider.read(source, mapper, flag);
    }
    public static java.util.ArrayList<Object> narrow(Iterable<Object> source, java.util.function.Function<Object,Object> mapper, boolean flag) {
        return (java.util.ArrayList<Object>)Provider.read(source, mapper, flag);
    }
    public static void ignored(Iterable<Object> source, java.util.function.Function<Object,Object> mapper, boolean flag) {
        Provider.read(source, mapper, flag);
    }
    public static java.util.List<Object> relay(Iterable<Object> source, java.util.function.Function<Object,Object> mapper) {
        return Provider.relay(source, mapper);
    }
    public static <T,R> java.util.List<R> typed(Iterable<T> source, java.util.function.Function<? super T,? extends R> mapper, boolean flag) {
        return Provider.read(source, mapper, flag);
    }
    public static java.util.List<Object> wide(Iterable<Object> source, java.util.function.Function<Object,Object> mapper, boolean flag, long stamp, double ratio) {
        return Provider.wide(source, mapper, flag, stamp, ratio);
    }
    public static java.util.List<Object> alias(Iterable<Object> source, java.util.function.Function<Object,Object> mapper, boolean flag) {
        return Facade.read(source, mapper, flag);
    }
}
