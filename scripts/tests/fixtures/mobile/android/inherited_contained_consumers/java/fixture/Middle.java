package fixture;
public interface Middle<M> extends Root<M> {
    Object request(Consumer<? super M> input, Consumer<? super Value> output);
}
