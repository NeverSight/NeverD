package fixture;

public final class Failure extends RuntimeException {
    public Failure() {
        Trace.failure = this;
        Trace.hit(3);
    }
}
