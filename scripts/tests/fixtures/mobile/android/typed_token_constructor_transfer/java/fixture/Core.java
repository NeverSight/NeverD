package fixture;
public final class Core<T extends Bound> {
    public final Output<T> output;
    public final Callback<? super Context, ? extends T> callback;
    public final Object payload;
    public final boolean done;
    public Core(Input<T> input, Callback<? super Context, ? extends T> callback) {
        Mapper.check(input, "input");
        Mapper.check(callback, "callback");
        this(Mapper.map(input), callback);
    }
    public Core(Output<T> output, Callback<? super Context, ? extends T> callback) {
        Mapper.escaping = this;
        this.output = output; this.callback = callback;
        payload = output.value;
        Mapper.observer.check(this, "target");
        if (Mapper.constructorFailure != null) throw Mapper.constructorFailure;
        done = true;
    }
}
