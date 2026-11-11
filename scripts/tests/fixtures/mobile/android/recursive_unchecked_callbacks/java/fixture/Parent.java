package fixture;
public abstract class Parent<P extends Parent<P>> extends Base<P> implements Updates {
    public byte[] forward(byte[] input) {
        forwardingCalls++;
        return update(input);
    }
}
