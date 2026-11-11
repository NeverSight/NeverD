package fixture;
public final class Transfer<X> {
    private final Input<X> input;
    public Transfer(Input<X> input) { this.input = input; }
    public Output<X> map() { return Mapper.map(input); }
}
