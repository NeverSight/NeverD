package fixture;
public class Provider {
    static { Trace.hit(1); }
    public static final <A, B, C> void write3(State state, int first, A a, int second, B b, int third, C c) {
        Trace.hit(2);
        int size = state.size;
        Operation operation = state.operations[state.used - 1];
        int base = size - operation.width();
        Object[] cells = state.cells;
        cells[base + first] = a;
        cells[base + second] = b;
        cells[base + third] = c;
        Trace.hit(6);
    }
    public static final <A, B, C, D> void write4(State state, int first, A a, int second, B b, int third, C c, int fourth, D d) {
        Trace.hit(2);
        int size = state.size;
        Operation operation = state.operations[state.used - 1];
        int base = size - operation.width();
        Object[] cells = state.cells;
        cells[base + first] = a;
        cells[base + second] = b;
        cells[base + third] = c;
        cells[base + fourth] = d;
        Trace.hit(6);
    }
}
