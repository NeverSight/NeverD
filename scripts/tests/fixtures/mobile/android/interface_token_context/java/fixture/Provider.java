package fixture;
import java.io.File;
import java.io.IOException;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <T extends CharSequence> T read(File context,Class<T> type,CharSequence... data)throws IOException {Trace.calls++;Trace.data=data;Trace.type=type;Trace.context=context;Trace.hit(1);Trace.action.run();if(Trace.checked!=null)throw Trace.checked;Trace.hit(2);return (T)Trace.result;}
}
