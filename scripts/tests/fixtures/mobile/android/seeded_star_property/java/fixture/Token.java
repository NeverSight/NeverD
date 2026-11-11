package fixture;
import java.io.IOException;
public interface Token<T, V> { V read(T value) throws IOException; }
