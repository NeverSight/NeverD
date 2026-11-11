package fixture;
public class Provider {
    public Object[] reference = new Object[1];
    public double[] wide = new double[1];
    public float[] narrow = new float[1];
    public long[] number = new long[1];
    public int[] integer = new int[1];
    public boolean[] flag = new boolean[1];
    public byte[] small = new byte[1];
    public char[] character = new char[1];
    public short[] medium = new short[1];
    public int after() {
        Object input = Trace.input;
        Trace.seen = input; Trace.hit(1);
        ((int[]) input)[0]++;
        reference[0] = null; wide[0] = Double.NEGATIVE_INFINITY; narrow[0] = -0.0f;
        number[0] = Long.MIN_VALUE; integer[0] = -1; flag[0] = false;
        small[0] = -1; character[0] = 'z'; medium[0] = -1;
        return 17;
    }
}
