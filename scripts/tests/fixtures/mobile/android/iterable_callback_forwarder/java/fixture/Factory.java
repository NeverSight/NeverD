package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <T extends Iterable<E>,E> T keep(T input,Matcher<E> matcher) {
        Trace.calls++;Trace.input=input;Trace.matcher=matcher;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (T)Trace.result;
    }
}
