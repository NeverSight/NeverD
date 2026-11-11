package fixture;
public final class Host<T extends Bound> {
    public int selector;
    Provider provider;
    Host(Provider provider) { this.provider = provider; }
    private int calculate(int base, T value) {
        switch (selector) {
            case 0: return base - provider.read(Trace.input(value));
            case 1: return base;
            default: throw new IllegalStateException();
        }
    }
    public int run(int base, T value) {
        Trace.hit(1);
        int result = calculate(base, value);
        Trace.hit(8);
        return result;
    }
}
