package fixture;
import java.util.function.Function;
import java.util.Map;
import java.util.stream.Collector;
import java.util.stream.Collectors;
public final class Core {
    private Core() {}
    public static Bucket<?,?,?> raw(Function<Object,Object> mapper, Bucket<Object,Object,Object> downstream) { return Factory.group(mapper,downstream); }
    public static Object ignored(Function<Object,Object> mapper, Bucket<Object,Object,Object> downstream) { Factory.group(mapper,downstream); return Trace.marker; }
    public static <T,K,A,D> Bucket<T,?,Map<K,D>> typed(Function<? super T,? extends K> mapper, Bucket<? super T,A,D> downstream) { return Factory.group(mapper,downstream); }
    public static Bucket<?,?,?> alias(Function<Object,Object> mapper, Bucket<Object,Object,Object> downstream) { return Facade.group(mapper,downstream); }
    public static Bucket<?,?,?> reverse(Function<Object,Object> mapper, Bucket<Object,Object,Object> downstream) { return Factory.reverse(downstream,mapper); }
    public static <T,K,A,D> Collector<T,?,Map<K,D>> sdk(Function<? super T,? extends K> mapper, Collector<? super T,A,D> downstream) { return Collectors.groupingBy(mapper,downstream); }
}
