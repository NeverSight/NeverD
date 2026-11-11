package fixture;

import java.util.LinkedHashMap;

public class Core extends LinkedHashMap<String, Object> {
    public int calls;
    public Object seen;
    public RuntimeException failure;
    public Core clone() {
        calls++;
        seen = this;
        if (failure != null) throw failure;
        return (Core) super.clone();
    }
}
