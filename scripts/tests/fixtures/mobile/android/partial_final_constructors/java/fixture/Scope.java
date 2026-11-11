package fixture;
public final class Scope {
    private Scope() {}
    private static final class Cell<E> {
        public E value;
        public Carrier<?> token;
        public final int stamp;
        private Cell() { Trace.hit(7); Trace.beforeCell(this); stamp = Trace.events; }
        public Cell(E value) { this(); this.value = value; this.token = Trace.box; }
        public static class Provider {
            static { Trace.hit(1); }
            @SuppressWarnings("unchecked")
            public <T> Carrier<Cell<T>> make(Carrier<T> input) {
                Trace.hit(2); Trace.seen = input; Trace.seenReceiver = this;
                input.touch();
                ((int[])Trace.input)[0]++;
                Trace.hit(3);
                return (Carrier<Cell<T>>)Trace.result;
            }
        }
    }
    public static Object create() { return new Cell.Provider(); }
    public static Object packet(Object value) { return new Cell<Object>(value); }
    public static Object value(Object packet) { return ((Cell<?>)packet).value; }
    public static Carrier<?> token(Object packet) { return ((Cell<?>)packet).token; }
    public static int stamp(Object packet) { return ((Cell<?>)packet).stamp; }
    public static Object read(Carrier<?> result) { return value(result.touch()); }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static Carrier<?> make(Object receiver, Carrier<?> input) {
        return ((Cell.Provider)receiver).make((Carrier)input);
    }
}
