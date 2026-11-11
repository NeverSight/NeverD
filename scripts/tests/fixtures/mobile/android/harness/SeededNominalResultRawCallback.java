import fixture.*;
public final class SeededNominalResultRawCallback implements Carrier {
    public Object expected;
    public float[] values;
    public int calls;
    public Object apply(Object incoming) {
        calls++; Trace.hit(3);
        if (incoming != expected || Trace.values != values)
            throw new AssertionError("seeded nominal result contract: raw callback identity");
        return "wrong callback result";
    }
}
