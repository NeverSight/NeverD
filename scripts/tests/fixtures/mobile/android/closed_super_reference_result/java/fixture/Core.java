package fixture;
import java.io.IOException;
import java.util.ArrayList;
public class Core extends Base<String,ArrayList<String>> {
    public ArrayList<String> submit(String value,ArrayList<String> items,IOException failure)throws IOException {
        Trace.hit(1);ArrayList<String> result=super.visit(value,items,failure);Trace.hit(4);return result;
    }
    public ArrayList<String> checked(Object value,Object items,IOException failure)throws IOException {
        Trace.hit(1);String argument=(String)value;ArrayList<String> carrier=(ArrayList<String>)items;
        ArrayList<String> result=super.visit(argument,carrier,failure);Trace.hit(4);return result;
    }
    public void discard(String value,ArrayList<String> items,IOException failure)throws IOException {
        Trace.hit(1);super.visit(value,items,failure);Trace.hit(4);
    }
}
