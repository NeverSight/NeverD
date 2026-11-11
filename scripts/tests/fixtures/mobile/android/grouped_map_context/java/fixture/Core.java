package fixture;
import java.util.*;
import java.util.function.Function;
import java.util.stream.Collector;
public final class Core {
    private Core() {}
    @SuppressWarnings("unchecked")
    public static Map direct(Collection values,Function classifier,Collector downstream,boolean flag) {return Factory.group(values,classifier,downstream,flag);}
    @SuppressWarnings("unchecked")
    public static Map simple(Collection values,Function classifier,Collector downstream) {return Factory.group(values,classifier,downstream,false);}
    @SuppressWarnings("unchecked")
    public static HashMap checked(Collection values,Function classifier,Collector downstream,boolean flag) {return (HashMap)Factory.group(values,classifier,downstream,flag);}
    @SuppressWarnings("unchecked")
    public static Object ignored(Collection values,Function classifier,Collector downstream,boolean flag) {Factory.group(values,classifier,downstream,flag);return Trace.marker;}
    public static <E,K,D> Map<K,D> relay(Collection<E> values,Function<E,K> classifier,Collector<E,?,D> downstream) {return Factory.group(values,classifier,downstream,false);}
}
