package fixture;
import java.io.IOException;
import java.nio.file.FileVisitResult;
public class Core extends Base<String> {
    public FileVisitResult submit(String value,IOException failure)throws IOException {
        Trace.hit(1);FileVisitResult result=super.visit(value,failure);Trace.hit(4);return result;
    }
    public FileVisitResult checked(Object value,IOException failure)throws IOException {
        Trace.hit(1);String argument=(String)value;FileVisitResult result=super.visit(argument,failure);Trace.hit(4);return result;
    }
}
