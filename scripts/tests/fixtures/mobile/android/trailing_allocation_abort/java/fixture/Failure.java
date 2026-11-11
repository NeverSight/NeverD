package fixture;
public final class Failure extends RuntimeException {
    static { Trace.step(7); }
    public Failure(String message) {
        super(message);
        Trace.aborted = this;
        Trace.step(8);
    }
}
