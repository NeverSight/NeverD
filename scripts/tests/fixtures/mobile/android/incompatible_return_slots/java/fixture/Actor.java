package fixture;

public class Actor<A> implements Leaf<A> {
    public A value;
    public RuntimeException failure;
    public int calls;
    public Actor() {}
    public Persistent<A> persistentRemove(Object key, Object value) {
        calls++; Trace.mark(4);
        if (failure != null) throw failure;
        return this;
    }
    public Persistent<A> persistentPut(A value) {
        calls++; Trace.mark(5);
        if (failure != null) throw failure;
        this.value = value;
        return this;
    }
    public Persistent<A> persistentClear() {
        calls++; Trace.mark(6);
        if (failure != null) throw failure;
        value = null;
        return this;
    }
    public static int __neverdReturnSlot() { return 7; }
}
