import fixture.*;
public final class AllocationInstanceCaller {
    public static Payload make(Provider provider, Object input) { return Core.make(provider, input); }
    public static Payload self(Provider provider, Object input) { return provider.self(input); }
}
