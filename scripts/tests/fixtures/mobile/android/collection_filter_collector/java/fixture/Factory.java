package fixture;
import java.util.Collection;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <T extends Collection<E>,E> T keep(T input,T target,Matcher<? super E> matcher) {
        Trace.calls++;Trace.input=input;Trace.target=target;Trace.matcher=matcher;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (T)Trace.result;
    }
}
