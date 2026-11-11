package fixture;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.List;

public class Provider {
    @SuppressWarnings("unchecked")
    public static <T> List<T> read(ByteBuffer context, Class<T> token) throws IOException {
        return Access.read(context,token);
    }
    public static final class Access {
        private Access() {}
        public static <T> List<T> read(ByteBuffer context, Class<T> token) throws IOException {
            return Provider.readValue(context,token);
        }
    }
    @SuppressWarnings("unchecked")
    private static <T> List<T> readValue(ByteBuffer context, Class<T> token) throws IOException {
        Trace.calls++;
        Trace.context=context;
        Trace.token=token;
        Trace.hit(1);
        Trace.action.run();
        if (Trace.checked!=null) throw Trace.checked;
        Object value=Trace.result;
        Trace.hit(2);
        return (List<T>)value;
    }
}
