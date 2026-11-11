package fixture;
public final class Owner<T> {
    public T value;
    public int calls;
    public RuntimeException failure;
    private final Root<T> view = new Root<T>() {
        protected T pending() {
            ++calls;
            if (failure != null) throw failure;
            return value;
        }
        public <U> U echo(U value) { return value; }
    };
    public Owner(T input) { value = input; }
    public T read() { return view.read(); }
    public Root<T> view() { return view; }
}
