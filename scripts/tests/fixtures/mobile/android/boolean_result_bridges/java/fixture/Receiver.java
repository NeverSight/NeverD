package fixture;

@SuppressWarnings("rawtypes")
public final class Receiver<S, V> implements Function {
    public boolean value;
    public int calls;
    public RuntimeException failure;

    public Receiver() {}

    public final boolean primitiveInvoke() {
        calls++;
        value = !value;
        if (failure != null) throw failure;
        return value;
    }

    public Object invoke() {
        return Boolean.valueOf(primitiveInvoke());
    }

    public int __neverdReturnSlot() { return 37; }
}
