package fixture;
public interface Carrier<T> extends java.io.Serializable {
  T read(java.nio.ByteBuffer input) throws java.io.IOException;
}
