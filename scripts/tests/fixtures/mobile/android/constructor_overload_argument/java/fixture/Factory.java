package fixture;
public final class Factory {
    private Factory() {}
    @SuppressWarnings("unchecked")
    public static <T> Box<T> empty() {
        Trace.hit(0, null, null, null);
        return (Box<T>)Trace.next;
    }
}
