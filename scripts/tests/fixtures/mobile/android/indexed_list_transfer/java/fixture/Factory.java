package fixture;
import java.util.List;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <E> List<List<E>> make(List<E> input,int index) {
        Trace.calls++;Trace.input=input;Trace.index=index;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (List<List<E>>)Trace.result;
    }
}
