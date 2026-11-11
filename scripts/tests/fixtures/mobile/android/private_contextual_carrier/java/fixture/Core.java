package fixture;
public final class Core {
    public static int events;
    public Context savedContext;
    public Callback savedCallback;
    public Carrier savedSeed;
    public Carrier savedResult;
    public Object marker;
    public RuntimeException failure;
    public int selected;
    private <T> Carrier<T> produce(int index, Context context, Callback<T> callback, Carrier<T> seed) {
        events = events * 5 + 1;
        savedContext = context;
        savedCallback = callback;
        savedSeed = seed;
        selected = index;
        marker = context.touch();
        Carrier<T> result = callback.create(context, index, seed);
        if (failure != null) throw failure;
        savedResult = result;
        events = events * 5 + 2;
        return result;
    }
    public static Carrier call(Core owner, int index, Context context, Callback callback, Carrier seed) {
        events = events * 5 + 3;
        Carrier result = owner.produce(index, context, callback, seed);
        events = events * 5 + 4;
        return result;
    }
    public static void discard(Core owner, int index, Context context, Callback callback, Carrier seed) {
        call(owner, index, context, callback, seed);
    }
    public static <R> Carrier<R> typed(Core owner, int index, Context context, Callback<R> callback, Carrier<R> seed) {
        events = events * 5 + 3;
        Carrier<R> result = owner.produce(index, context, callback, seed);
        events = events * 5 + 4;
        return result;
    }
}
