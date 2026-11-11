package fixture;
public class Core<T, U> {
    public int calls;
    public Object seen;
    public RuntimeException failure;
    public <A extends T, B extends A> A[] copy(A[] input, B tag, Sink<? super A> sink) {
        calls++; seen = tag;
        if (failure != null) throw failure;
        sink.accept(tag, input);
        return input;
    }
    public T value(T input) { return input; }
}
