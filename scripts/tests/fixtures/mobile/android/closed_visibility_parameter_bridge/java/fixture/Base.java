package fixture;
abstract class Base<S extends Base<S, A, B>, A extends First<S>, B extends Second<S>> extends Root {
    public void apply(A value) {
        Trace.hit(3);
        Trace.receiver = this;
        Trace.argument = value;
        Trace.action.run();
        Trace.hit(4);
    }
}
