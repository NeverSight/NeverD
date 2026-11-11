package fixture;

public final class Owner {
    public static <T extends Flag<? extends Value>> Object make(Token<T> token) { return new Worker<T>(token); }
    public static <T extends Flag<? extends Value>> Object another(Token<T> token) { return new Worker<T>(token); }
    public static final class Worker<T extends Flag<? extends Value>> implements Reader {
        public final Token<T> token;
        public Worker(Token<T> token) { this.token = token; }
        public T read() { return token.read(); }
        public Worker<T> copy() { return new Worker<T>(token); }
    }
}
