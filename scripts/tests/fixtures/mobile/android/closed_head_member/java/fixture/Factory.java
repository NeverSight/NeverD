package fixture;
import java.io.IOException;
import java.util.Map;
public interface Factory {
    <T> Map<String, T> query(Class<T> type) throws IOException;
}
