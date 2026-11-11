package fixture;
public final class Child<S, V> extends Base<S> {
    public S value;
    public Child() {}
    public void set(S value) {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        this.value = value;
    }
    @Override public void set$contract(S value) { set(value); }
}
