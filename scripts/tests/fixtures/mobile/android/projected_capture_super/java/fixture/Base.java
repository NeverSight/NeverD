package fixture;
public abstract class Base {
    public final int arity;
    public final Sink<Object> completion;
    public Base(int arity,Sink<Object> completion) {
        Trace.partial=this;Trace.hit(1);
        this.arity=arity;this.completion=completion;
        Trace.seen=read();Trace.hit(2);
        completion.accept(this);Trace.hit(3);
    }
    public abstract UpperBound read();
    public abstract Base spawn(Sink<Object> completion);
}
