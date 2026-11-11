package fixture;
public final class Guard {
    static { Trace.event(1); }
    public static void check(Object receiver, String label) {
        Trace.checked = receiver;
        Trace.event(2);
        if (!"owned receiver".equals(label)) throw new AssertionError("label");
        if (receiver == null && !Trace.allowNull) throw Trace.nullFailure;
    }
}
