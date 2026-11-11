package fixture;
import java.io.IOException;
import java.util.Map;
public class Impl implements Factory {
    public <T> Map<String, T> query(Class<T> type) throws IOException {
        Trace.receiver = this;
        Trace.token = type;
        Trace.calls++;
        Trace.hit(1);
        Trace.action.run();
        Trace.hit(2);
        return Trace.result;
    }
}
