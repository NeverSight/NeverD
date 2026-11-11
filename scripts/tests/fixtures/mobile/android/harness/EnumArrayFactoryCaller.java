import fixture.Client;
public final class EnumArrayFactoryCaller {
    public static Object call(Enum[] values) { return Client.call(values); }
    public static Thread.State checked(Enum[] values) { return Client.checked(values); }
}
