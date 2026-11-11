package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public Object result;
    public Context context;
    public Callback<? super Float> callback;
    public Box<Float, Vector> box;
    public Receiver() {}
    public Object apply(Context context, float first, float second,
                        Callback<? super Float> callback, Box<Float, Vector> box) {
        Trace.events = Trace.events * 10 + 3;
        Trace.first = Float.floatToRawIntBits(first);
        Trace.second = Float.floatToRawIntBits(second);
        this.context = context; this.callback = callback; this.box = box;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        if (callback != null) callback.accept(Float.valueOf(first));
        return result;
    }
    @SuppressWarnings("unchecked")
    public Object apply(Context context, Object first, Object second, Callback callback, Box box) {
        return apply(context, ((Number) first).floatValue(), ((Number) second).floatValue(), callback, box);
    }
}
