package fixture;
import java.io.IOException;
import java.nio.file.FileVisitResult;
public class Base<T> {
    public FileVisitResult visit(T value,IOException failure)throws IOException {
        Trace.hit(2);Trace.value=value;Trace.error=failure;
        Trace.action.run();
        if(failure!=null)throw failure;
        Trace.hit(3);return Trace.result;
    }
}
