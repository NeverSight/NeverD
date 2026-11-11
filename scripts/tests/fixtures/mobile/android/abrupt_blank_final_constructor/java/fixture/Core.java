package fixture;

public final class Core extends Base {
    public final Object value;
    public final long number;
    public final boolean enabled;

    private Core() {
        Trace.hit(2);
        throw new Failure();
    }

    private Core(Throwable failure, long number) throws Throwable {
        this.number = number;
        Trace.hit(2);
        throw failure;
    }

    public Core(Object value, long number, boolean enabled) {
        Trace.hit(2);
        this.value = value;
        this.number = number;
        this.enabled = enabled;
        Trace.hit(4);
    }
}
