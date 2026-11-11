package fixture;

public class Derived extends Actor<Object> {
    public Derived() {}
    public Persistent<Object> persistentRemove(Object key, Object value) {
        calls += 2; Trace.mark(7);
        if (failure != null) throw failure;
        return this;
    }
    public Persistent<Object> persistentPut(Object value) {
        calls += 2; Trace.mark(8);
        if (failure != null) throw failure;
        this.value = value;
        return this;
    }
    public Persistent<Object> persistentClear() {
        calls += 2; Trace.mark(9);
        if (failure != null) throw failure;
        value = null;
        return this;
    }
}
