package fixture;

// Only the owned generic class Signature and bridge flags are encoded later.
// Target/bridge names, descriptors and all Code bytes remain unchanged.
@SuppressWarnings("rawtypes")
public final class Receiver implements Function {
    public Marker seen;
    public int value;
    public int calls;
    public RuntimeException failure;
    public void invoke(Marker marker, int value) {
        Trace.events = Trace.events * 10 + 1;
        ++calls;
        this.seen = marker;
        this.value = value;
        if (failure != null) throw failure;
    }
    public Object invoke(Object marker, Object value) {
        invoke((Marker) marker, ((Number) value).intValue());
        return kotlin.Unit.INSTANCE;
    }
}
