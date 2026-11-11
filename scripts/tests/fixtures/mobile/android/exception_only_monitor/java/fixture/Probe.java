package fixture;

import java.io.IOException;

public interface Probe {
    void observe(Object lock) throws IOException;
}
