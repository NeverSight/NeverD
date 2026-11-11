package fixture;
public abstract class Parent<A, B> {
    public Class<B> token;
    public String label;
    public Parent(Class<B> token, String label) {
        Trace.hit(1); Trace.receiver = this;
        this.token = token; Trace.token = token; Trace.hit(2);
        this.label = label; Trace.label = label;
        Trace.hit(3); Trace.action.run(); Trace.hit(4);
    }
}
