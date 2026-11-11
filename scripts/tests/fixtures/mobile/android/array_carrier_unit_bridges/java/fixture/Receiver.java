package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public float[] seen;
    public int calls;
    public Object invoke(Object value) {
        apply(((Carrier) value).unwrap());
        return kotlin.Unit.INSTANCE;
    }
    public final void apply(float[] value) {
        Trace.events = Trace.events * 10 + 3;
        seen = value;
        Trace.first = value != null && value.length != 0 ? Float.floatToRawIntBits(value[0]) : 0;
        calls++;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        if (value != null && value.length != 0)
            value[0] = Float.intBitsToFloat(Float.floatToRawIntBits(value[0]) ^ 0x80000000);
    }
}
