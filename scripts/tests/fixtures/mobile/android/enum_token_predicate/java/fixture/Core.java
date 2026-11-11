package fixture;
public final class Core {
    private Core() {}
    public static <Value extends Enum<Value>> boolean call(Class<Value> type, String key) {
        return Provider.test(type,key);
    }
    public static boolean checked(Object type, Object key) {
        return call((Class)type,(String)key);
    }
    public static <Value extends Enum<Value>> boolean negate(Class<Value> type, String key) {
        return !call(type,key);
    }
}
