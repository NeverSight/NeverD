package fixture;
public interface Api<E> {
    void add(E input);
    void set(E input);
    void remove();
    Void narrowAdd(Object input);
    Void narrowSet(Object input);
    Void narrowRemove();
    Object read();
}
