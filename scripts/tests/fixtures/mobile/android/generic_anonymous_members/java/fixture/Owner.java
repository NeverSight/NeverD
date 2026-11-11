package fixture;

public final class Owner<T> {
    public T value;
    public int calls;
    public RuntimeException failure;
    private final Reader<T> reader = new Reader<T>() {
        public T read() {
            ++calls;
            if (failure != null) throw failure;
            return value;
        }
        public <T> T echo(T value) { return value; }
    };
    public Owner(T value) { this.value = value; }
    public T read() { return reader.read(); }
    public Reader<T> reader() { return reader; }
}
