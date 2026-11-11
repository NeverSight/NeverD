package fixture;
public final class Core {
    public final Box<Value> box;
    public final Callback<? super Value,? super Label,Value> callback;
    public final Value result;
    public Core(Box<Value> box,Callback<? super Value,? super Label,Value> callback) {
        Trace.partial=this;Trace.hit(4);
        this.box=box;Trace.hit(5);
        this.callback=callback;Trace.hit(6);
        Value result=callback.apply(Trace.value,Trace.label);
        this.result=result;Trace.hit(8);
    }
    private Core(long bits,Callback<? super Value,? super Label,Value> callback) { this(new Box<Value>(bits),callback); }
    public static Core create(long bits,Callback<? super Value,? super Label,Value> callback) { return new Core(bits,callback); }
}
