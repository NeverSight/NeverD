package fixture;

public class PlainState extends java.util.concurrent.atomic.AtomicBoolean implements View {
    private final java.util.List<String> entries;
    private final java.util.function.Consumer<String> callback;

    public PlainState(boolean initial) {
        super(initial);
        entries = null;
        callback = null;
    }

    public PlainState(boolean initial, java.util.List<String> entries,
                      java.util.function.Consumer<String> callback) {
        super(initial);
        this.entries = entries;
        this.callback = callback;
        callback.accept("owned construction");
    }

    public java.util.List<String> metadata() { return entries; }
    public <M> M echo(M item) { return item; }
    public void notify(String item) { callback.accept(item); }
    public String toString() { return "owned parent"; }
}
