package fixture;

public final class Receiver<E> {
    public E value;
    public int reads;
    public RuntimeException failure;
    public Receiver(E value) { this.value = value; }
    public Object read() {
        ++reads;
        if (failure != null) throw failure;
        return value;
    }
    public <R> Mapper pick(Mapper supplied) {
        read();
        return supplied;
    }
    public static <Unused> Mapper pick$default(Receiver receiver, Mapper supplied, int mask, Object marker) {
        if ((mask & 1) != 0) {
            supplied = new Mapper() {
                public Object apply(Box<Object> box) { return box.value; }
            };
        }
        receiver.read();
        return supplied;
    }
}
