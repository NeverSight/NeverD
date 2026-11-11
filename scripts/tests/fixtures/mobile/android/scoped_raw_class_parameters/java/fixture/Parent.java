package fixture;
public abstract class Parent<P extends View> {
    public int calls;
    public Object marker;
    public View input;
    public Object after;
    public RuntimeException failure;
    public boolean read(Object marker, P input, Object after) {
        calls++;
        this.marker = marker;
        this.input = input;
        this.after = after;
        if (marker instanceof int[]) ((int[]) marker)[0] += 3;
        if (failure != null) throw failure;
        return input != null;
    }
}
