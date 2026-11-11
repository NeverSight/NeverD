package fixture;
public final class Creator {
    public void make() {
        Object value;
        Trace.result = new Outer(((value = Trace.holder.value) == null | true)
                                 ? new Inner(value) : null);
    }
}
