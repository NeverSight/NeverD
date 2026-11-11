import fixture.*;
public final class ContravariantCarrierCaller {
    public static Carrier<?> call(Carrier<?> input) { return Client.call(input); }
}
