package fixture;
public interface Root<T> {
    Object request(Consumer<? super T> input, Consumer<? super Value> output);
}
