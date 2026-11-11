package fixture;
public abstract class Parent<P extends Parent<P>> {
    <T> Parent(T seed, Token<T> token) {
        Trace.hit(3);
        Trace.receiver = this;
        Trace.seed = seed;
        Trace.token = token;
        float value = token.read(seed);
        Trace.observed = value;
        Trace.hit(5);
        token.write(seed, value);
        Trace.hit(7);
        Trace.action.run();
    }
    Parent(Value value) {
        Trace.receiver = this;
        Trace.seed = value;
    }
}
