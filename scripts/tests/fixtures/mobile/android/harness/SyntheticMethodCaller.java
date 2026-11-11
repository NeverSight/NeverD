import fixture.*;
public final class SyntheticMethodCaller {
    public static Object relay(Object value) { return Provider.relay(value); }
    public static Object effect(Object value) { return Provider.effect(value); }
    public static Box allocate(Object value) { return Provider.allocate(value); }
    public static long wide(long value) { return Provider.wideRelay(value); }
}
