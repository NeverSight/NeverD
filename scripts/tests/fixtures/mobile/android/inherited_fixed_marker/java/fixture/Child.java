package fixture;
public final class Child extends Core {
    public long stamp;
    public Child(Value value,long bits,boolean enabled,Sink sink,kotlin.jvm.internal.DefaultConstructorMarker marker) {
        super(value,bits,enabled,sink,marker);stamp=bits;Trace.hit(5);
    }
    public Value current() { Trace.hit(4);return value; }
}
