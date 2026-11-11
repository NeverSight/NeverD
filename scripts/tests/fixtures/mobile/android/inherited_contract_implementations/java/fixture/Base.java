package fixture;
public abstract class Base implements Api {
    public int calls;
    public Object seen, seenToken;
    public Probe observer;
    public RuntimeException failure;
    public <R, S> R echo(Class<R> token, R input) {
        calls++; seen = input; seenToken = token;
        if (failure != null) throw failure;
        observer.observe(input, token);
        return input;
    }
    public Carrier<Number, String> inspect(Carrier<Number, String> input) {
        calls++; seen = input; seenToken = null;
        if (failure != null) throw failure;
        observer.observe(input, null);
        return input;
    }
}
