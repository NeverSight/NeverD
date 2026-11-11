package fixture;
public final class Core {
    public static Target make(Provider owner, int input, long bits, float floating, double fraction) {
        boolean saved;
        Target result = new Target(saved = owner.value, saved, input, input, bits, floating, fraction);
        Trace.afterFlag = saved; Trace.afterWritten = true;
        Trace.tail = bits; Trace.floatBits = Float.floatToRawIntBits(floating);
        Trace.doubleBits = Double.doubleToRawLongBits(fraction);
        return result;
    }
    public static Target makeStatic(Provider owner, int input, long bits, float floating, double fraction) {
        boolean saved;
        Target result = new Target(saved = Provider.staticFlag, saved, input, input, bits, floating, fraction);
        Trace.afterFlag = saved; Trace.afterWritten = true;
        Trace.tail = bits; Trace.floatBits = Float.floatToRawIntBits(floating);
        Trace.doubleBits = Double.doubleToRawLongBits(fraction);
        return result;
    }
}
