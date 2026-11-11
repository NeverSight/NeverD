package fixture;
public class Parent {
    public Parent() { Trace.leaked = (Core)this; }
}
