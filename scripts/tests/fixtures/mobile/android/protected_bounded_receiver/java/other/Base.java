package other;
import api.Bound;
import fixture.Trace;
public abstract class Base {
    @SuppressWarnings("unchecked")
    protected final <T extends Bound> T choose(T input) {
        Trace.receiver=this;Trace.input=input;Trace.hit(1);
        input.touch();Trace.hit(2);
        T result=(T)Trace.output;
        Trace.result=result;Trace.hit(3);return result;
    }
}
