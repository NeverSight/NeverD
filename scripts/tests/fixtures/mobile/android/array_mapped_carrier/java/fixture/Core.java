package fixture;
import java.util.Set;
import java.util.HashSet;
import java.util.function.Function;
public final class Core {
    private Core() {}
    public static Set<Object> raw(Object[] source, Function<Object,Object> mapper) { return Provider.read(source,mapper); }
    public static HashSet<Object> narrow(Object[] source, Function<Object,Object> mapper) { return (HashSet<Object>)Provider.read(source,mapper); }
    public static Object ignored(Object[] source, Function<Object,Object> mapper) { Provider.read(source,mapper); return Trace.marker; }
    public static Set<Object> relay(Object[] source, Function<Object,Object> mapper) { return Provider.relay(source,mapper); }
    public static <T,R> Set<R> typed(T[] source, Function<? super T,? extends R> mapper) { return Provider.read(source,mapper); }
    public static Set<Object> alias(Object[] source, Function<Object,Object> mapper) { return Facade.read(source,mapper); }
}
