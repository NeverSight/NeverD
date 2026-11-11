package fixture;

public final class Receiver<E> {
    public E value;
    public int reads;
    public RuntimeException failure;
    public static Object[] slots;
    public static int sinkCalls;
    public Object slot(int index) { return slots[index]; }
    public Receiver(E value) { this.value = value; }
    public Object read() {
        ++reads;
        if (failure != null) throw failure;
        return value;
    }
    public <R> Mapper pick(Mapper supplied) {
        Object a0 = slot(0);
        Object a1 = slot(1);
        Object a2 = slot(2);
        Object a3 = slot(3);
        Object a4 = slot(4);
        Object a5 = slot(5);
        Object a6 = slot(6);
        Object a7 = slot(7);
        Object a8 = slot(8);
        Object a9 = slot(9);
        Object a10 = slot(10);
        Object a11 = slot(11);
        Object a12 = slot(12);
        Object a13 = slot(13);
        Object a14 = slot(14);
        Object a15 = slot(15);
        Object a16 = slot(16);
        read();
        sink(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16);
        return supplied;
    }
    public static <Unused> Mapper pick$default(Receiver receiver, Mapper supplied, int mask, Object marker) {
        if ((mask & 1) != 0) {
            supplied = new Mapper() {
                public Object apply(Box<Object> box) { return box.value; }
            };
        }
        Object a0 = receiver.slot(0);
        Object a1 = receiver.slot(1);
        Object a2 = receiver.slot(2);
        Object a3 = receiver.slot(3);
        Object a4 = receiver.slot(4);
        Object a5 = receiver.slot(5);
        Object a6 = receiver.slot(6);
        Object a7 = receiver.slot(7);
        Object a8 = receiver.slot(8);
        Object a9 = receiver.slot(9);
        Object a10 = receiver.slot(10);
        Object a11 = receiver.slot(11);
        Object a12 = receiver.slot(12);
        Object a13 = receiver.slot(13);
        Object a14 = receiver.slot(14);
        Object a15 = receiver.slot(15);
        Object a16 = receiver.slot(16);
        receiver.read();
        sink(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16);
        return supplied;
    }
    public static void sink(Object a0, Object a1, Object a2, Object a3, Object a4, Object a5, Object a6, Object a7, Object a8, Object a9, Object a10, Object a11, Object a12, Object a13, Object a14, Object a15, Object a16) {
        if (a0 != slots[0]) throw new AssertionError("slot identity");
        if (a1 != slots[1]) throw new AssertionError("slot identity");
        if (a2 != slots[2]) throw new AssertionError("slot identity");
        if (a3 != slots[3]) throw new AssertionError("slot identity");
        if (a4 != slots[4]) throw new AssertionError("slot identity");
        if (a5 != slots[5]) throw new AssertionError("slot identity");
        if (a6 != slots[6]) throw new AssertionError("slot identity");
        if (a7 != slots[7]) throw new AssertionError("slot identity");
        if (a8 != slots[8]) throw new AssertionError("slot identity");
        if (a9 != slots[9]) throw new AssertionError("slot identity");
        if (a10 != slots[10]) throw new AssertionError("slot identity");
        if (a11 != slots[11]) throw new AssertionError("slot identity");
        if (a12 != slots[12]) throw new AssertionError("slot identity");
        if (a13 != slots[13]) throw new AssertionError("slot identity");
        if (a14 != slots[14]) throw new AssertionError("slot identity");
        if (a15 != slots[15]) throw new AssertionError("slot identity");
        if (a16 != slots[16]) throw new AssertionError("slot identity");
        ++sinkCalls;
    }
}
