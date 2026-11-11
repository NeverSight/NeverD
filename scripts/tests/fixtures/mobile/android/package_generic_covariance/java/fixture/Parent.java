package fixture;
abstract class Parent<P> {
    public int calls;
    public Object marker;
    public Result<P> inherited;
    public RuntimeException failure;
    Result<P> copy() {
        calls++;
        if (marker instanceof int[]) ((int[]) marker)[0] += 1;
        if (failure != null) throw failure;
        return inherited;
    }
}
