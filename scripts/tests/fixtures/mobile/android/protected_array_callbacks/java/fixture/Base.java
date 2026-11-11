package fixture;

public abstract class Base<P, Q, R> {
    public int trace;

    public Base() { cancel(null); }

    protected abstract R run(P... values);
    protected void cancel(R result) { trace = trace * 10 + 8; }
    protected void finish(R result) { trace = trace * 10 + 9; }

    public R dispatch(P... values) {
        trace = trace * 10 + 1;
        return run(values);
    }

    public void dispatchCancel(R result) {
        trace = trace * 10 + 2;
        cancel(result);
    }

    public void dispatchFinish(R result) {
        trace = trace * 10 + 3;
        finish(result);
    }
}
