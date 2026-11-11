package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public Context context;
    public Callback callback;
    public Boolean result;
    public Object invoke(Object first, Object second, Object third) {
        Context context = (Context) first;
        long scalar = ((CarrierB) second).unwrap();
        Callback callback = (Callback) third;
        return apply(context, scalar, callback);
    }
    public final Boolean apply(Context context, long scalar,
                               Callback<? super Context, Result> callback) {
        Trace.events = Trace.events * 10 + 3;
        this.context = context;
        this.callback = callback;
        Trace.second = scalar;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return result;
    }
}
