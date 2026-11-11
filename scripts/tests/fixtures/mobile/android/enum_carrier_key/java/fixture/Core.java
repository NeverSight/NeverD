package fixture;
public final class Core {
    private Core() {}
    public static <Value extends Enum<Value>,Key> Value call(Pair<Value,Key> input, Key key) {
        return Provider.select(input,key);
    }
    public static <Value extends Enum<Value>,Key> Value fallback(Pair<Value,Key> input, Key key, Value fallback) {
        return Provider.choose(input,key,fallback);
    }
    public static Enum checked(Object input, Object key, Object fallback) {
        return fallback((Pair)input,key,(Enum)fallback);
    }
}
