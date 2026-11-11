package fixture;
public final class Provider {
    public volatile Object payload, extra;
    public Object fallback() {
        Trace.fallbackCalls++;
        Trace.step(3);
        return Trace.fallbackValue;
    }
    public long finish(long bits) {
        Trace.tailCalls++;
        Trace.step(6);
        Trace.inputBits = bits;
        payload = Trace.changedPayload;
        extra = Trace.changedExtra;
        Trace.step(7);
        return Trace.tailBits;
    }
}
