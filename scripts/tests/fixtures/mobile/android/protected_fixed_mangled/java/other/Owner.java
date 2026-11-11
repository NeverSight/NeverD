package other;
import api.*;
import fixture.Trace;
public abstract class Owner extends Base {
    protected final Object chooseValue(Press scope,long bits,Sink<? super Value> sink) {
        Trace.receiver=this;Trace.scope=scope;Trace.bits=bits;Trace.sink=sink;Trace.hit(1);
        scope.touch();Trace.hit(2);
        sink.accept(Trace.value);Trace.hit(3);
        Object result=Trace.output;Trace.result=result;Trace.hit(6);return result;
    }
}
