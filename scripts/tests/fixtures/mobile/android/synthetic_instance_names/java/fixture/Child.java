package fixture;
public class Child extends Core {
    protected void after() { Trace.receiver = this; Trace.hit(4); }
}
