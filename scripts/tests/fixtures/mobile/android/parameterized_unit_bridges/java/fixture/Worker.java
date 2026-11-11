package fixture;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class Worker implements Action {
    public int calls;
    public boolean last;
    public Callback<Boolean> callback;
    public RuntimeException failure;
    public void run(Callback<Boolean> callback) {
        ++calls;
        this.callback = callback;
        Trace.events = Trace.events * 10 + 1;
        if (failure != null) throw failure;
        last = callback.value().booleanValue();
    }
    public Object invoke(Object callback) {
        run((Callback<Boolean>) callback);
        return kotlin.Unit.INSTANCE;
    }
    public static void call(Worker worker, Callback<Boolean> callback) {
        worker.run(callback);
    }
}
