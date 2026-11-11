package fixture;
import java.io.IOException;

public interface Probe {
    void initialize();
    Object select(Object value) throws IOException;
    void allocated(Object[] values) throws IOException;
    Object element(int index, Object value) throws IOException;
    Object[] finish(Object[] values, Object first, Object second) throws IOException;
    void constructed(Result value) throws IOException;
}
