package fixture;
import java.io.IOException;
@SuppressWarnings({"unchecked","rawtypes"})
public class Core {
    public DerivedApi receiver;
    public Core(DerivedApi receiver) { this.receiver=receiver; }
    public <R> R read(Class<R> token) throws IOException { return receiver.fetch(token); }
    public static Object call(Core core,Class<?> token) throws IOException {
        Trace.hit(1); Object result=core.read((Class)token);
        Trace.result=result; Trace.hit(7); Trace.after.run(); Trace.hit(8); return result;
    }
    public static String checked(Core core,Class<?> token) throws IOException {
        Trace.hit(1); String result=(String)core.read((Class)token);
        Trace.result=result; Trace.hit(7); Trace.after.run(); Trace.hit(8); return result;
    }
}
