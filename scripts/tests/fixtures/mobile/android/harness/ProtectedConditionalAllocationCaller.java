import fixture.*;
public final class ProtectedConditionalAllocationCaller {
    public static Target make(Maker maker,Provider provider,Object seed) {
        return maker.make(provider,seed);
    }
}
