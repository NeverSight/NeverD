package fixture;
import java.util.Set;
import java.util.function.Function;
public class Provider extends Base {
    public Provider() {}
    @SuppressWarnings("unchecked")
    public static <T,R> Set<R> read(T[] source, Function<? super T,? extends R> mapper) {
        Trace.calls++;
        Trace.stage = 1;
        Trace.source = source;
        Trace.mapper = mapper;
        T item = source[Trace.index];
        Trace.item = item;
        Trace.stage = 2;
        R mapped = mapper.apply(item);
        Trace.mapped = mapped;
        Trace.stage = 3;
        Object saved = Trace.store;
        Trace.action.run();
        source[Trace.index] = (T)saved;
        Trace.stage = 4;
        return (Set<R>)Trace.value;
    }
    public static <T,R> Set<R> relay(T[] source, Function<? super T,? extends R> mapper) {
        Trace.relays++;
        return read(source,mapper);
    }
}
