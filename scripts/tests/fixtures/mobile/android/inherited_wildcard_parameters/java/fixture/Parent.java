package fixture;
public abstract class Parent<E extends Pair<? extends K,? extends V>,K,V> extends Base<E> {
    public boolean typedRemove(Pair<?,?> entry) { return super.remove(entry); }
    public final boolean checked(Object entry) { if (!(entry instanceof Pair)) return false; return typedRemove((Pair<?,?>) entry); }
    public boolean call(Object entry) { return checked(entry); }
}
