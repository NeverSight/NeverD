package fixture;
import java.util.LinkedHashMap;
public final class Core {
    private Core() {}
    public static <Value extends Enum<Value>> LinkedHashMap<String,Value> call(Class<Value> type) {
        return Provider.collect(type);
    }
    public static LinkedHashMap checked(Object type) {
        return call((Class) type);
    }
    public static <Value extends Enum<Value>> boolean contains(Class<Value> type, Object key) {
        return call(type).containsKey(key);
    }
}
