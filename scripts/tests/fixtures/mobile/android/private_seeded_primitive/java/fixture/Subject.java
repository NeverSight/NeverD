package fixture;
public final class Subject {
    public boolean z;
    public int i, state;
    public long j;
    public float f;
    public double d;
    public Subject() {}
    @SuppressWarnings("unchecked")
    public boolean runZ(Callback callback, Object value) { return selectZ(callback, value); }
    private final <T> boolean selectZ(Callback<? super T> callback, T value) {
        Trace.hit(1, this, callback, value);
        callback.accept(value);
        state++;
        boolean result = z;
        Trace.hit(2, this, callback, value);
        return result;
    }
    @SuppressWarnings("unchecked")
    public int runI(Callback callback, Object value) { return selectI(callback, value); }
    private final <T> int selectI(Callback<? super T> callback, T value) {
        Trace.hit(1, this, callback, value);
        callback.accept(value);
        state++;
        int result = i;
        Trace.hit(2, this, callback, value);
        return result;
    }
    @SuppressWarnings("unchecked")
    public long runJ(Callback callback, Object value) { return selectJ(callback, value); }
    private final <T> long selectJ(Callback<? super T> callback, T value) {
        Trace.hit(1, this, callback, value);
        callback.accept(value);
        state++;
        long result = j;
        Trace.hit(2, this, callback, value);
        return result;
    }
    @SuppressWarnings("unchecked")
    public float runF(Callback callback, Object value) { return selectF(callback, value); }
    private final <T> float selectF(Callback<? super T> callback, T value) {
        Trace.hit(1, this, callback, value);
        callback.accept(value);
        state++;
        float result = f;
        Trace.hit(2, this, callback, value);
        return result;
    }
    @SuppressWarnings("unchecked")
    public double runD(Callback callback, Object value) { return selectD(callback, value); }
    private final <T> double selectD(Callback<? super T> callback, T value) {
        Trace.hit(1, this, callback, value);
        callback.accept(value);
        state++;
        double result = d;
        Trace.hit(2, this, callback, value);
        return result;
    }
}
