package fixture;
@SuppressWarnings("rawtypes")
public final class Carrier extends Root<Carrier> implements Comparable {
    public short value;
    public Carrier(short input) { value = input; }
    public final short read() {
        Effects.unboxCalls++;
        if (Effects.unboxFailure != null) throw Effects.unboxFailure;
        return value;
    }
    public int compareScalar(short other) {
        Effects.targetCalls++;
        Effects.lastSelf = this;
        if (Effects.targetFailure != null) throw Effects.targetFailure;
        return Integer.compare(value, other);
    }
    public int compareTo(Object input) {
        Carrier checked = (Carrier) input;
        short unboxed = checked.read();
        return compareScalar(unboxed);
    }
}
