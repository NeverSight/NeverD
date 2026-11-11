package fixture;
public class Target {
    public final boolean[] booleanValues;
    public final byte[] byteValues;
    public final char[] charValues;
    public final short[] shortValues;
    public final int[] intValues;
    public final long[] longValues;
    public final float[] floatValues;
    public final double[] doubleValues;
    public final int before, after;
    static { Trace.hit(9); Trace.seed = 41; }
    public Target(int before, boolean[] booleanValues, byte[] byteValues, char[] charValues, short[] shortValues, int[] intValues, long[] longValues, float[] floatValues, double[] doubleValues, int after) {
        this.booleanValues = booleanValues;
        this.byteValues = byteValues;
        this.charValues = charValues;
        this.shortValues = shortValues;
        this.intValues = intValues;
        this.longValues = longValues;
        this.floatValues = floatValues;
        this.doubleValues = doubleValues;
        this.before = before; this.after = after;
        Trace.published = this;
        Trace.hit(8);
    }
}
