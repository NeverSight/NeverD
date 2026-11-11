package fixture;
import java.io.IOException;
public interface Api { <E> E fetch(Class<E> token) throws IOException; }
