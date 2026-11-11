package fixture;
public final class Child extends Core<Object> {
    public long stamp;
    public Child(Object value,long bits,Sink sink,kotlin.jvm.internal.DefaultConstructorMarker marker) {
        super(value,bits,sink,marker);stamp=bits;Trace.hit(5);
    }
    public Object current() { Trace.hit(4);return value; }
}
