import fixture.Client;

public final class NestedPairCaller {
    public static Object call(Iterable<?> first, Iterable<?> second) { return Client.call(first, second); }
    public static String checkedKey(Iterable<?> first, Iterable<?> second) { return Client.checkedKey(first, second); }
    public static String checkedValue(Iterable<?> first, Iterable<?> second) { return Client.checkedValue(first, second); }
}
