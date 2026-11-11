package fixture;
public final class Context {
    public Object next;
    public Context(Object next) { this.next = next; }
    public Object fetch() { Trace.hit(5, this, next, null); return next; }
}
