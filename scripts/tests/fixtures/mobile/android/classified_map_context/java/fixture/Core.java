package fixture;
import java.util.*;
import java.util.function.Function;
@SuppressWarnings("unchecked")
public final class Core {
    private Core() {}
    public static Map direct(Collection values,Function classifier,boolean flag) {return Factory.group(values,classifier,flag);}
    public static Map simple(Collection values,Function classifier) {return Factory.group(values,classifier,false);}
    public static HashMap checked(Collection values,Function classifier,boolean flag) {return (HashMap)Factory.group(values,classifier,flag);}
    public static Object ignored(Collection values,Function classifier,boolean flag) {Factory.group(values,classifier,flag);return Trace.marker;}
    public static <E,K> Map<K,List<E>> relay(Collection<E> values,Function<E,K> classifier) {return Factory.group(values,classifier,false);}
    public static Map value(Collection values,Function classifier,boolean flag) {return Factory.value(values,classifier,flag);}
    public static <E,K> Map<K,E> valueRelay(Collection<E> values,Function<E,K> classifier) {return Factory.value(values,classifier,false);}
}
