package fixture;

public final class DirectTransform implements Contract {
    public static int calls;
    private final Cursor<Pair<? extends Box<String>, ? extends Box<Object>>> values;
    public DirectTransform(Cursor<Pair<? extends Box<String>, ? extends Box<Object>>> values) {
        this.values = values;
    }
    public Object map(Object input) {
        Pair<? extends Box<String>, ? extends Box<Object>> typed =
                (Pair<? extends Box<String>, ? extends Box<Object>>) input;
        ++calls;
        return typed.get().read();
    }
    public Cursor<Pair<? extends Box<String>, ? extends Box<Object>>> items() { return values; }
}
