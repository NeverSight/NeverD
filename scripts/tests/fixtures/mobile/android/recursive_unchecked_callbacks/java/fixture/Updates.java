package fixture;
public interface Updates {
    byte[] update(byte[] input);
    default byte[] adapt(byte[] input) { return update(input); }
}
