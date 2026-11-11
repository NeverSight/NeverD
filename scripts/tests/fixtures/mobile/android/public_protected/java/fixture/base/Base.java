package fixture.base;

public abstract class Base<P, R> {
    public Base() {}

    protected abstract R value(P[] values);

    public R run(P[] values) {
        return value(values);
    }
}
