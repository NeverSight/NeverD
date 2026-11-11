package fixture;
public abstract class Core<T> {
    public final T value;
    public final long bits;
    public final Sink<T> sink;
    private Core(T value,long bits,Sink<T> sink) {
        Trace.partial=this;Trace.hit(1);this.value=value;this.bits=bits;this.sink=sink;
        Trace.hit(2);sink.accept(current());Trace.hit(3);
    }
    @SuppressWarnings("unchecked")
    public Core(Object value,long bits,Sink sink,kotlin.jvm.internal.DefaultConstructorMarker marker) {
        this((T)value,bits,(Sink<T>)sink);
    }
    public abstract T current();
}
