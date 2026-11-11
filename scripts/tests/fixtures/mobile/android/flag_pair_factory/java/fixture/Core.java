package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Object direct(boolean flag) {return Factory.make(flag);}
    public static Object capacity(int capacity,boolean flag) {return Factory.make(capacity,flag);}
    public static LinkedHashMap checked(boolean flag) {return (LinkedHashMap)Factory.make(flag);}
    public static Object ignored(boolean flag) {Factory.make(flag);return Trace.marker;}
    public static <K,V> HashMap<K,V> relay(boolean flag) {return Factory.make(flag);}
    public static String key(boolean flag) {return Factory.<String,Object>make(flag).keySet().iterator().next();}
    public static String value(boolean flag) {return Factory.<Object,String>make(flag).get(Trace.key);}
}
