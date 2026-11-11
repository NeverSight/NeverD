package fixture;
public interface Provider extends AutoCloseable {
    Object get();
    void close();
}
