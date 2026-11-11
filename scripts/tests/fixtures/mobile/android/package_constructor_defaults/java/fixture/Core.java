package fixture;
final class Core {
    final Carrier<Object> state;
    final Core next;
    final Object initial;
    public Core(Carrier<? extends Object> state, Core next) {
        Trace.hit(2, this);
        this.state = (Carrier)state;
        Trace.hit(3, this);
        this.next = next;
        Trace.hit(4, this);
        this.initial = this.state.current();
        Trace.core = this;
        Trace.hit(5, this);
    }
    public Core(Carrier state, Core next, int mask, Object ignored) {
        this(state, (mask & 2) != 0 ? null : next);
    }
}
