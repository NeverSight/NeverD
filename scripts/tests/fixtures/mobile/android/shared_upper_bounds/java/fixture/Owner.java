package fixture;

public final class Owner<T extends UpperBound> {
    public Object make(Token<T> token) { return new Worker<T>(token); }
    public Object another(Token<T> token) { return new Worker<T>(token); }
    public static final class Worker<T extends UpperBound> implements Reader<T> {
        public final Token<T> token;
        public Worker(Token<T> token) { this.token = token; }
        public T read() {
            T value = token.read();
            value.inspect();
            return value;
        }
        public Worker<T> copy() { return new Worker<T>(token); }
    }
}
