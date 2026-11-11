package fixture;
public class Root {
    public Root() { Trace.hit(3); Trace.self = this; Trace.root.run(); }
}
