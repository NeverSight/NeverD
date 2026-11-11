package fixture;

@SuppressWarnings("rawtypes")
public final class ArgWorker implements ArgAction {
    public int calls;
    public Value value;
    public RuntimeException failure;
    public void run(Value value) {
        ++calls;
        this.value = value;
        if (failure != null) throw failure;
    }
    public Object invoke(Object value) {
        run((Value) value);
        return kotlin.Unit.INSTANCE;
    }
    public static void call(ArgWorker worker, Value value) { worker.run(value); }
}
