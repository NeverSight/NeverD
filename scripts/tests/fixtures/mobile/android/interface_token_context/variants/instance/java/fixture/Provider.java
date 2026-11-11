package fixture;

import java.io.IOException;
import java.security.Key;

public class Provider {
    @SuppressWarnings("unchecked")
    public <T extends CharSequence> T read(Key context, Class<T> type) throws IOException {
        Trace.calls++;
        Trace.receiver=this;
        Trace.context=context;
        Trace.type=type;
        Trace.hit(1);
        Trace.action.run();
        if (Trace.checked!=null) throw Trace.checked;
        Trace.hit(2);
        return (T)Trace.result;
    }
}
