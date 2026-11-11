package fixture;
import java.io.File;
import java.util.Collection;
public class Child extends Base {
    public Object value;
    public Child(Collection<File> values){super(new String[0]);Trace.values=values;Trace.hit(3);Trace.childAction.run();Trace.hit(4);value=Trace.result;}
    public Child(String[] data,Collection<File> values){super(data);Trace.values=values;Trace.hit(3);Trace.childAction.run();Trace.hit(4);value=Trace.result;}
}
