import fixture.Child;
import fixture.Trace;
import fixture.View;
public final class ScopedSuperCaller {
    public static View<String> run(Child<String> receiver, int from, int to) {
        View<String> output = receiver.subList(from, to);
        Trace.hit(6);
        return output;
    }
}
