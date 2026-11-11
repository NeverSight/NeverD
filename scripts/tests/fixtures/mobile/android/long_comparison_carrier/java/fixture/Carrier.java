package fixture;
@SuppressWarnings("rawtypes")
public final class Carrier extends Root<Carrier> implements Comparable {
    public long value;
    public Carrier(long input) { value = input; }
    public final long read() {
        Effects.unboxCalls++;
        Effects.lastUnboxed = this;
        if (Effects.unboxFailure != null) throw Effects.unboxFailure;
        return value;
    }
    public static int compareValues(long left, long right) {
        Effects.primitiveCalls++;
        Effects.lastLeft = left; Effects.lastRight = right;
        if (Effects.primitiveFailure != null) throw Effects.primitiveFailure;
        return Long.compare(left, right);
    }
    public int compareScalar(long other) {
        Effects.targetCalls++;
        Effects.lastSelf = this;
        Effects.lastInput = other;
        if (Effects.targetFailure != null) throw Effects.targetFailure;
        return compareValues(value, other);
    }
    public int compareTo(Object input) {
        Carrier checked = (Carrier) input;
        long unboxed = checked.read();
        return compareScalar(unboxed);
    }
}
