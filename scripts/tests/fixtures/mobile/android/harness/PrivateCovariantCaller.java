import fixture.Client;
import fixture.Producer;
import fixture.Provider;
public final class PrivateCovariantCaller {
    @SuppressWarnings("rawtypes")
    public static String call(int route, Provider provider, Producer producer, String context) {
        if (route == 0) return provider.call(producer, context);
        if (route == 1) return Client.read(provider, producer, context);
        throw new AssertionError("invalid owned route");
    }
}
