package fixture;

public final class Token<T> {
    public T value;
    public Token(T value) { this.value = value; }
    public T read() {
        State.read();
        return value;
    }
}
