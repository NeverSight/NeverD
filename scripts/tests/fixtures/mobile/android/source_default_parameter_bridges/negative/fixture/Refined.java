package fixture;

public interface Refined extends Ordered {
    @Override
    int compare(Ordered other);
}
