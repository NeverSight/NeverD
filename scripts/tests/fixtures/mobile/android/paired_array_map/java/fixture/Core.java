package fixture;
import java.util.Map;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Map direct(Object[] keys,Object[] values,boolean flag) {return Factory.make(keys,values,flag);}
    public static <K,V> Map<K,V> relay(K[] keys,V[] values,boolean flag) {return Factory.make(keys,values,flag);}
    public static Object ignored(Object[] keys,Object[] values,boolean flag) {Factory.make(keys,values,flag);return Trace.marker;}
    public static Map<String,String> strings(String[] keys,String[] values,boolean flag) {return Factory.make(keys,values,flag);}
}
