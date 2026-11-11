package fixture;
public interface Contract {
    interface Creator<T> { T next(); }
}
