package fixture;
public class Owner<Value extends Owner.Item> {
    public interface Item { int read(); }
    public Value value;
    public Owner(Value seed) { this.value = seed; }
    public Value echo(Value input) {
        Trace.receiver = this; Trace.hit(1);
        value = input; Trace.last = input; Trace.hit(2);
        Trace.number = input.read(); Trace.hit(3);
        return value;
    }
    public <Item> Item local(Item input) { return input; }
}
