package fixture;

public class Provider {
    public Provider() {}
    @SuppressWarnings("unchecked")
    public static <T,R> java.util.List<R> read(Iterable<T> source, java.util.function.Function<? super T,? extends R> mapper, boolean flag) {
        Trace.calls++;
        Trace.stage = 1;
        Trace.source = source;
        Trace.mapper = mapper;
        Trace.flag = flag;
        java.util.Iterator<T> cursor = source.iterator();
        Trace.iterator = cursor;
        T item = cursor.next();
        Trace.item = item;
        Trace.stage = 2;
        R mapped = mapper.apply(item);
        Trace.mapped = mapped;
        Trace.stage = 3;
        Trace.action.run();
        Trace.stage = 4;
        return (java.util.List<R>)Trace.value;
    }
    public static <T,R> java.util.List<R> relay(Iterable<T> source, java.util.function.Function<? super T,? extends R> mapper) {
        Trace.relays++;
        return read(source, mapper, false);
    }
    public static <T,R> java.util.List<R> wide(Iterable<T> source, java.util.function.Function<? super T,? extends R> mapper, boolean flag, long stamp, double ratio) {
        Trace.wides++;
        Trace.stamp = stamp;
        Trace.ratio = ratio;
        return read(source, mapper, flag);
    }
}
