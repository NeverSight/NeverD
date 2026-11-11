package fixture;
public class Resolved extends Redeclared {
    public Resolved(Sink sink) { super(sink); }
    protected void touch(Object marker) {
        touches += 3; seen = marker;
        if (failure != null) throw failure;
    }
    public Object item() { return seen; }
    public <R> R identity(R value) {
        touches += 2;
        if (failure != null) throw failure;
        return value;
    }
}
