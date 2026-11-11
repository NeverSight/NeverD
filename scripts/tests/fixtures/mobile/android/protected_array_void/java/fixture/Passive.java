package fixture;

public class Passive extends Base {
    public <U> void forward(U[] input, int tag) {
        mark(tag);
        consume(input);
    }
}
