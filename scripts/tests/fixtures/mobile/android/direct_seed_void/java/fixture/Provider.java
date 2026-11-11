package fixture;
public class Provider {
    static { Trace.hit(1); }
    public static final <A, B> void write(State state, int first, A a, int second, B b) {
        Trace.hit(2);
        int size = state.size;
        Operation operation = state.operations[state.used - 1];
        int base = size - operation.width();
        Object[] cells = state.cells;
        cells[base + first] = a;
        cells[base + second] = b;
        Trace.hit(6);
    }
    public static final <A, B> void writeWide(State state, int first, A a, int second, B b, long bits) {
        Trace.bits = bits;
        write(state, first, a, second, b);
    }
}
