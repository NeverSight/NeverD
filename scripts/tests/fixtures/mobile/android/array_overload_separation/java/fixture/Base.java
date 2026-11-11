package fixture;
import java.io.File;
import java.util.Collection;
public class Base {
    public Base(String... data){Trace.calls++;Trace.data=data;Trace.hit(1);Trace.action.run();Trace.hit(2);}
    public Base(Collection<File> values){Trace.siblings++;Trace.values=values;Trace.hit(9);}
}
