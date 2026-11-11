package fixture;
public final class Provider {
    static { Trace.step(4); }
    public static <T extends Node> void visit(T start, Fn<? super T,? extends Node.Companion.Action> operation) {
        Trace.before(start);
        Node.Companion.Action choice = operation.apply(start);
        Trace.after(start, choice);
    }
    public static void visit(Object start, Fn<?,?> operation) { Trace.step(9); }
}
