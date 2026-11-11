package fixture;
import java.io.IOException;
@SuppressWarnings("unchecked")
public class Provider implements DerivedApi {
    public int calls;
    public <T> T fetch(Class<T> token) throws IOException {
        calls++; Trace.hit(3); Trace.receiver=this; Trace.token=token;
        Trace.action.run(); Trace.hit(4);
        if(Trace.checked!=null) throw Trace.checked;
        Object value=Trace.value; Trace.produced=value; Trace.hit(5);
        Trace.providerAfter.run(); Trace.hit(6);
        return (T)value;
    }
}
