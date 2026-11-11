package fixture;
public class Core extends Builder<Payload> {
    public int finished;
    public Core(Factory factory, int id) {
        Guard.check(factory);
        Carrier<? extends Payload> argument = factory.get(Leaf.class);
        Guard.check(argument);
        super(argument, id);
        finished = 9;
    }
}
