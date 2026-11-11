package fixture;

import java.io.IOException;

public interface Probe {
    Object observe(Object self, Object values, Object context) throws IOException;
}
