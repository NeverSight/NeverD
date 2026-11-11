package fixture;

@SuppressWarnings("rawtypes")
public final class MutableSelector implements Selector {
    public Object result;
    public Object last;
    public int calls;
    public RuntimeException failure;
    public MutableSelector(Object result) { this.result = result; }
    public Object select(Object input) {
        ++calls;
        last = input;
        if (failure != null) throw failure;
        return result;
    }
}
