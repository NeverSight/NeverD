package fixture;
import java.util.function.Function;
import java.util.stream.Collector;
import java.util.stream.Collectors;
public final class Core {
    private Core() {}
    public static Bucket<?,?,?> raw(Function<Object,Object> mapper, Bucket<Object,Object,Object> downstream) { return Factory.map(mapper,downstream); }
    public static Object ignored(Function<Object,Object> mapper, Bucket<Object,Object,Object> downstream) { Factory.map(mapper,downstream); return Trace.marker; }
    public static <T,U,A,R> Bucket<T,?,R> typed(Function<? super T,? extends U> mapper, Bucket<? super U,A,R> downstream) { return Factory.map(mapper,downstream); }
    public static Bucket<?,?,?> alias(Function<Object,Object> mapper, Bucket<Object,Object,Object> downstream) { return Facade.map(mapper,downstream); }
    public static Bucket<?,?,?> reverse(Function<Object,Object> mapper, Bucket<Object,Object,Object> downstream) { return Factory.reverse(downstream,mapper); }
    public static <T,U,A,R> Collector<T,?,R> sdk(Function<? super T,? extends U> mapper, Collector<? super U,A,R> downstream) { return Collectors.mapping(mapper,downstream); }
}
