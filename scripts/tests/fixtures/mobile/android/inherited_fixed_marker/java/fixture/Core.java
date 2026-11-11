package fixture;
public abstract class Core extends Base implements Tagged {
    public final Value value;
    public final long bits;
    public final boolean enabled;
    public final Sink<Value> sink;
    private Core(Value value,long bits,boolean enabled,Sink<Value> sink) {
        Trace.partial=this;Trace.hit(1);this.value=value;this.bits=bits;this.enabled=enabled;this.sink=sink;
        Trace.hit(2);sink.accept(current());Trace.hit(3);
    }
    @SuppressWarnings("unchecked")
    public Core(Value value,long bits,boolean enabled,Sink sink,kotlin.jvm.internal.DefaultConstructorMarker marker) {
        this(value,bits,enabled,(Sink<Value>)sink);
    }
    public abstract Value current();
}
