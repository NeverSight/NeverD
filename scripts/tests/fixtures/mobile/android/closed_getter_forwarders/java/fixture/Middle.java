package fixture;
public class Middle extends Base {
    public Box<Throwable> data;
    public Middle() { super(); }
    public Box<Throwable> getItems() {
        State.calls++;
        State.receiver = this;
        if (State.failure != null) throw State.failure;
        return data;
    }
    public Box items() { return getItems(); }
}
