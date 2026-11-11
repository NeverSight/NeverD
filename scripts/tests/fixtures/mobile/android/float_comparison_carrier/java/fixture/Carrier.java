package fixture;
@SuppressWarnings("rawtypes")
public final class Carrier extends Root<Carrier> implements Comparable {
    public float value;
    public Carrier(float input) { value = input; }
    public final float read() {
        Effects.unboxCalls++;
        if (Effects.unboxFailure != null) throw Effects.unboxFailure;
        return value;
    }
    public static int compareValues(float left, float right) {
        Effects.primitiveCalls++;
        if (Effects.collapseNaN && (Float.isNaN(left) || Float.isNaN(right))) return 0;
        return Float.compare(left, right);
    }
    public int compareScalar(float other) {
        Effects.targetCalls++;
        Effects.lastSelf = this;
        if (Effects.targetFailure != null) throw Effects.targetFailure;
        return compareValues(value, other);
    }
    public int compareTo(Object input) {
        Carrier checked = (Carrier) input;
        float unboxed = checked.read();
        return compareScalar(unboxed);
    }
}
