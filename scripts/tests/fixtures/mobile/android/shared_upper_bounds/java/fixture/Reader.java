package fixture;

public interface Reader<T extends UpperBound> {
    T read();
}
