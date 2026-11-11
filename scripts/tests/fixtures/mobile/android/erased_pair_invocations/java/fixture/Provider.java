package fixture;
public class Provider implements API {
    static { Trace.hit(1); }
    public <I, O> Box<I> make(Transform<I, O> transform, Callback<O> callback) {
        return make(transform, callback, Trace.flags);
    }
    public <I, O> Box<I> make(Transform<I, O> transform, Callback<O> callback, long flags) {
        Trace.flags = flags;
        Trace.receiver = this;
        Trace.hit(2);
        I value = transform.input();
        O decoded = transform.decode(value);
        callback.accept(decoded);
        Trace.hit(6);
        return new Box<I>(value);
    }
}
