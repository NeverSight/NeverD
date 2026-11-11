package fixture;
public final class Core implements Api {
    public long value;
    public Core(long input) { value = input; }
    public int compareTo(Api other) { return Long.compare(value, ((Core) other).value); }
    public final long scalarShift(long delta) {
        Effects.scalarCalls++; Effects.events = Effects.events * 10 + 1;
        Effects.lastSelf = this; Effects.lastDelta = delta;
        if (Effects.scalarFailure != null) throw Effects.scalarFailure;
        Effects.lastScalar = value + delta;
        return Effects.lastScalar;
    }
    public static final Core box(long input) {
        Effects.boxCalls++; Effects.events = Effects.events * 10 + 2;
        Effects.lastBoxInput = input;
        if (Effects.boxFailure != null) throw Effects.boxFailure;
        Core result = Effects.returnNull ? null : new Core(input);
        Effects.lastBox = result;
        return result;
    }
    public Api shift(long delta) { return box(scalarShift(delta)); }
    public Parent rootShift(long delta) { return box(scalarShift(delta)); }
}
