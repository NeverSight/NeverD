import fixture.*;
public final class SeededNominalResultCallback implements Carrier<int[], Float> {
    public Object expected;
    public float[] values;
    public Float answer;
    public Error failure;
    public boolean mutate;
    public int calls;
    public Float apply(int[] incoming) {
        calls++; Trace.hit(3);
        if (incoming != expected || Trace.values != values)
            throw new AssertionError("seeded nominal result contract: callback identity");
        if (failure != null) throw failure;
        if (incoming != null) incoming[0]++;
        if (mutate) values[0] = Float.intBitsToFloat(0x7fc00456);
        return answer;
    }
}
