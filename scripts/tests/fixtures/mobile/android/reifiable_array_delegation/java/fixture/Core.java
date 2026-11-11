package fixture;
import java.io.File;
import java.nio.file.Path;
public class Core {
    public Object value;
    public Core(File source,Class<?>... data){this(source.toPath(),data);}
    public Core(String ignored,Class<?>... data){this((Path)null,data);}
    public Core(Path source,Class<?>... data){Trace.calls++;Trace.path=source;Trace.data=data;Trace.hit(3);Trace.targetAction.run();Trace.hit(4);value=Trace.result;}
}
