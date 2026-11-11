package fixture;
public final class Primary extends Provider {
    @Override public <I, O> void accept(int flags, Pair<I, O> pair, I input, Object token, long bits) {
        Trace.hit(3); Trace.receiver = this; calls++;
        O result = pair.apply(input, flags, token, bits);
        Trace.result = result;
        Trace.hit(5);
        if (result != null) ((int[]) (Object) result)[0] += 1;
        Trace.hit(6);
    }
}
