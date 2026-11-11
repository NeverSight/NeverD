import fixture.Client;
public final class ArraySeedCaller {
    public static Object call(Object[] values) { return Client.call(values); }
    public static String checked(Object[] values) { return Client.checked(values); }
}
