package fixture;
abstract class Base<P extends Input> extends Root {
    P value;
    protected Base() { super(); }
    public P first() {
        Trace.hit(3);
        Trace.receiver = this;
        P result = value;
        Trace.before = result;
        Trace.action.run();
        Trace.hit(4);
        return result;
    }
}
