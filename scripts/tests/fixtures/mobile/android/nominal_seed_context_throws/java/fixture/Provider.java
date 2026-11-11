package fixture;
import java.io.IOException;
public class Provider {
    public <T> Context read(String context,T value,boolean left,boolean right)throws IOException {
        Trace.calls++;Trace.hit(1);Trace.actualReceiver=this;Trace.captured=value;Trace.context=context;Trace.left=left;Trace.right=right;
        int length=context.length();Trace.observed=length;Trace.hit(2);
        Trace.action.run();Trace.hit(3);return Trace.result;
    }
}
