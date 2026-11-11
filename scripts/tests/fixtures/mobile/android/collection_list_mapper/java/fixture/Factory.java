package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <E> List<List<E>> group(Collection<E> input,Classifier<E> classifier) {
        Trace.calls++;Trace.input=input;Trace.classifier=classifier;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (List<List<E>>)Trace.result;
    }
}
