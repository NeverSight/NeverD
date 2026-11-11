package fixture;
public class Context {
    static { Trace.hit(7);Trace.ready=true; }
    public final Object token;
    public final int snapshot;
    public Object value;
    public Context(int observed) { token=Trace.token;snapshot=observed;Trace.hit(2);Trace.context=this; }
}
