package fixture;
import java.io.IOException;
public class Implementation extends Provider {
    @Override public <T> Provider accept(Token<T> token, T value) throws IOException {
        Trace.calls++; Trace.actualReceiver=this; Trace.captured=value; Trace.token=token; Trace.hit(2);
        Trace.action.run();
        if (Trace.failure != null) throw Trace.failure;
        Trace.hit(3); return Trace.result;
    }
}
