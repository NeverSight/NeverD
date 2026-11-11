package fixture;

public final class Token<T extends Flag<? extends Value>> {
    public T value;
    public int reads;
    public RuntimeException failure;
    public Token(T value) { this.value = value; }
    public T read() {
        ++reads;
        if (failure != null) throw failure;
        return value;
    }
}
