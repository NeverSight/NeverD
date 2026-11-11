package fixture;
@SuppressWarnings({"unchecked", "rawtypes"})
public final class Provider {
    static { Trace.step(4); }
    private static <T extends Node> void visit(T start, Fn<? super T,? extends Node.Companion.Action> operation) {
        Trace.before(start);
        Node.Companion.Action choice = operation.apply(start);
        Trace.after(start, choice);
    }
    public static void forward(Node start, Fn operation) { visit(start, operation); }
    public static void visit(Object start, Fn<?,?> operation) { Trace.step(9); }
}
