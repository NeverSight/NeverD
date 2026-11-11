package fixture;
@SuppressWarnings({"unchecked", "rawtypes"})
public final class Owner<T> {
    private static final class Hidden<H> {}
    private static <T> Atom<Hidden<T>> keep(Atom<Hidden<T>> input) {
        Trace.input = input; Trace.hit(1);
        if (Trace.observer != null) Trace.observer.observe(input);
        Trace.hit(2);
        Atom<Hidden<T>> result = Trace.useAlternate ? (Atom)Trace.alternate : input;
        Trace.result = result; Trace.hit(3);
        return result;
    }
    public static Atom make(Atom input) { return keep(input); }
    public static Atom create() { return keep(new Atom(null)); }
}
