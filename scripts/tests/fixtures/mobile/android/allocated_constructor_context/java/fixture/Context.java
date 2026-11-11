package fixture;
public class Context {
    public final Object token;
    public Object value;
    public Context() { token=Trace.token;Trace.hit(2);Trace.context=this; }
}
