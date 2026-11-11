package fixture;
import java.io.IOException;
public class Provider {
    public <T> int read(Context context)throws IOException {
        Trace.calls++;Trace.hit(1);Trace.actualReceiver=this;Trace.captured=context;
        int value=context.value;Trace.observed=value;Trace.hit(2);
        Trace.action.run();Trace.hit(3);return Trace.result;
    }
}
