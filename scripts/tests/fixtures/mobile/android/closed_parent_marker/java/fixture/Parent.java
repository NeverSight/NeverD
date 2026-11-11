package fixture;
public class Parent<T extends Token> {
    public Parent() { Trace.parent=this;Trace.parentCount++;Trace.hit(7); }
}
