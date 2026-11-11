package fixture;

public abstract class Parent implements Observer {
    public int observed;
    public Parent() {}
    public void event(int value) { Trace.mark(8); observed = value; }
}
