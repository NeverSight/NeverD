package fixture;

public class Problem extends Exception {
    static {
        Trace.hit(8);
        if (Trace.failInit) throw Trace.constructorFailure;
    }

    public Problem(String message) {
        super(message);
        Trace.hit(3);
        if (Trace.failConstructor) throw Trace.constructorFailure;
        Trace.published = this;
    }
}
