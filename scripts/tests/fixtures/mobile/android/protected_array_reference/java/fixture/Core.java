package fixture;
public final class Core extends Base<boolean[], Result> {
    public int calls;
    public Object seen;
    public Result last;
    public RuntimeException failure;
    protected Result convert(boolean[] input) {
        calls++; seen = input;
        if (failure != null) throw failure;
        Result result = new Result(input); last = result; return result;
    }
}
