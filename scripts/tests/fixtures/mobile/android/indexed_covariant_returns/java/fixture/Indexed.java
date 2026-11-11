package fixture;
public interface Indexed<I> {
    Base<I> create(int lower, int upper);
}
