package fixture;
public class Core extends Builder<Payload> {
    public int finished;
    public Core(Leaf value, int id) {
        Guard.check(value);
        Carrier<? extends Payload> argument = value;
        super(argument, id);
        finished = 9;
    }
}
