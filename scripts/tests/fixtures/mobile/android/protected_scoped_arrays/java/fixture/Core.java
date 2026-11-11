package fixture;
public final class Core<E> extends Base<E[], E> {
    public int calls;
    public Object seen;
    public Result<E> last;
    public RuntimeException failure;
    protected Result<E> convert(E[] input) {
        calls++; seen = input;
        if (failure != null) throw failure;
        Result<E> result = new Result<E>(input); last = result; return result;
    }
}
