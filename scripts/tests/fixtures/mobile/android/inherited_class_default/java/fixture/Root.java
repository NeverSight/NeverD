package fixture;
public class Root<R, S> {
    public R stored;
    public R lastInput;
    public Cell<R, S> pair;
    public int valueCalls;
    public int lookupCalls;
    public RuntimeException failure;
    public Root() {}
    public R value(R next) {
        valueCalls++;
        lastInput = next;
        if (failure != null) throw failure;
        R old = stored;
        stored = next;
        return old;
    }
    public Cell<R, S> lookup(R input) {
        lookupCalls++;
        lastInput = input;
        if (failure != null) throw failure;
        return pair;
    }
}
