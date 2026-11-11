import fixture.*;
public final class ProtectedAllocationCaller {
    public static Object make(int variant) { return variant == 0 ? Maker.observed(Trace.token) : Maker.fallback(); }
}
