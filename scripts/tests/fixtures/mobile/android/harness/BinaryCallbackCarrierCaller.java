import fixture.Carrier;
import fixture.Client;
import fixture.Comparison;
public final class BinaryCallbackCarrierCaller {
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static Carrier call(Carrier source, Comparison comparison) { return Client.call(source, comparison); }
    public static String take(Carrier<String> result) { return result.next(); }
}
