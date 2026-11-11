package fixture;
public final class Creator {
    public Input input;
    public Target make() {
        return new Target((Value) Provider.read(this.input), new Inner(this.input));
    }
}
