package fixture;

public final class Token<T extends UpperBound> {
    public T value;
    public RuntimeException failure;
    public Token(T value) { this.value = value; }
    public T read() {
        if (failure != null) throw failure;
        return value;
    }
}
