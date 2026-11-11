package fixture;
public class Base {
    public boolean flag;
    public Base() { Trace.step(3); }
    public void setFlag(boolean value) {
        Trace.step(8);
        flag = value;
        Trace.observe((Core) this, 9);
    }
}
