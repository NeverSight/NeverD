import fixture.Child;
import java.net.URI;
import java.net.URISyntaxException;
public final class InheritedProtectedAllocationCaller {
    public static URI call(Child receiver,Object value)throws URISyntaxException {return receiver.make(value);}
}
