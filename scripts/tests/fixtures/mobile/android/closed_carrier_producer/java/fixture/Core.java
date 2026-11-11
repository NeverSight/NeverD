package fixture;
import java.util.Collection;
public class Core {
    @SuppressWarnings({"unchecked","rawtypes"})
    public static Object call(Provider receiver,Collection<String> labels,Context context,Producer<?> producer) throws java.io.IOException {
        Trace.hit(1); Object result=receiver.read(labels,context,(Producer)producer);
        Trace.hit(7); Trace.result=result; Trace.after.run(); Trace.hit(8); return result;
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static String checked(Provider receiver,Collection<String> labels,Context context,Producer<?> producer) throws java.io.IOException {
        Trace.hit(1); String result=(String)receiver.read(labels,context,(Producer)producer);
        Trace.hit(7); Trace.result=result; Trace.after.run(); Trace.hit(8); return result;
    }
}
