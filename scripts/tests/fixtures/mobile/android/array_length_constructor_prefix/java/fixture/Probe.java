package fixture;

import java.io.IOException;

public interface Probe {
    int begin();
    void constructed(Core value) throws IOException;
    void after(Core value) throws IOException;
}
