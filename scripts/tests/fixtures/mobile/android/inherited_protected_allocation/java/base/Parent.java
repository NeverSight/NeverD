package base;
import fixture.Trace;
public abstract class Parent<T> implements java.io.Serializable {
    protected String text(Object value){Trace.calls++;Trace.value=value;Trace.hit(1);Trace.action.run();Trace.hit(2);return Trace.result;}
}
