package fixture;
public final class Core {
    public static Target make(int input, long bits, float floating, double fraction) {
        Value saved;
        Target result = new Target(input, input, 2, saved = (Value) Provider.value, saved, bits, floating, fraction);
        Trace.after = saved;
        Trace.tail = bits;
        Trace.floatBits = Float.floatToRawIntBits(floating);
        Trace.doubleBits = Double.doubleToRawLongBits(fraction);
        return result;
    }
}
