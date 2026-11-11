package fixture;

import java.util.HashMap;
import java.util.Map;

public class Store<A, B> extends HashMap<A, B> {
    static {
        Trace.initializations++;
        if (Trace.initFailure != null) throw Trace.initFailure;
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    public Store() {
        Trace.allocations++;
        Trace.created = this;
        Trace.stage = 1;
        Trace.action.run();
        ((Map) this).put(Trace.key, Trace.value);
        Trace.stage = 2;
    }
}
