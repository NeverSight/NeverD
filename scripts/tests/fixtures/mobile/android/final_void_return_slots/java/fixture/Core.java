package fixture;
public final class Core<T> extends Middle<T> {
    public int voidCalls, voidState;
    public Object voidInput;
    public RuntimeException voidFailure;
    public final void append(T item) {
        voidCalls++;
        voidState = voidState * 3 + 1;
        voidInput = item;
        if (item instanceof int[]) ((int[]) item)[0] += 5;
        if (voidFailure != null) throw voidFailure;
    }
    public void twice(T item) { append(item); append(item); }
}
