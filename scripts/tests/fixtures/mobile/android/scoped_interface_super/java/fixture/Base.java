package fixture;
public abstract class Base<E> extends java.util.AbstractList<E> {
    static { Trace.hit(8); }
    @Override public java.util.List<E> subList(int from, int to) {
        Trace.wrong = true;
        Trace.hit(7);
        return null;
    }
}
