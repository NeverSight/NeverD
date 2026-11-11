import fixture.Client;

public final class NestedPairConsumerCaller {
    public static Object call(Iterable<?> values) { return Client.call(values); }
    public static String checkedKey(Iterable<?> values) { return Client.checkedKey(values); }
    public static String checkedValue(Iterable<?> values) { return Client.checkedValue(values); }
}
