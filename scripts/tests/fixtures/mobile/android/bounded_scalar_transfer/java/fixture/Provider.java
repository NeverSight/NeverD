package fixture;

import java.io.IOException;
import java.util.List;

public class Provider {
    @SuppressWarnings("unchecked")
    public static <T extends Number> List<T> read(List<T> values, int first, int second, boolean flag) throws IOException {
        Trace.calls++;
        Trace.values=values;
        Trace.first=first;
        Trace.second=second;
        Trace.flag=flag;
        Trace.hit(1);
        Trace.action.run();
        if (Trace.checked!=null) throw Trace.checked;
        Trace.hit(2);
        return (List<T>)Trace.result;
    }
}
