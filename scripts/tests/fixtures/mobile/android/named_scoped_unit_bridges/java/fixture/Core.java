package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Core<E> extends Parent<E> implements BiTask {
    public int calls;
    public E value;
    public Throwable error;
    public RuntimeException failure;
    public void run(E value, Throwable error) {
        calls++;
        this.value = value;
        this.error = error;
        if (failure != null) throw failure;
    }
    public Object apply(Object first, Object second) {
        run((E) first, (Throwable) second);
        return kotlin.Unit.INSTANCE;
    }
    public void forward(E first, Throwable second) { run(first, second); }
}
