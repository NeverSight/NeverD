package fixture;
public class Problem extends Exception {
    static { Trace.hit(1); }
    public Problem(String message) {
        super(message); Trace.hit(2);
        Trace.published = this; Trace.hit(3);
    }
}
