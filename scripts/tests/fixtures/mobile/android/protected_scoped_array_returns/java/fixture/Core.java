package fixture;
public final class Core<E> extends Base<Result<E>, E[]> {
    public int calls;
    public Object seen;
    public E[] last;
    public RuntimeException failure;
    protected E[] convert(Result<E> input) {
        calls++; seen = input;
        if (failure != null) throw failure;
        E[] result = input.read(); last = result; return result;
    }
}
