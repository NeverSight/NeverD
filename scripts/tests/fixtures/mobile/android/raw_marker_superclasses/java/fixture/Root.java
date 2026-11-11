package fixture;

public class Root<P> implements ArityView<P> {
    public int rootCalls;
    public RuntimeException rootFailure;
    public Root() {}

    public int arity() {
        rootCalls++;
        if (rootFailure != null) throw rootFailure;
        return 3;
    }
}
