package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Caller {
    public static Object key(Function receiver, Object input) { return receiver.key(input); }
    public static View source(Function receiver) { return receiver.source(); }
}
