import api.*;
public class ProtectedOverloadIdentifiersProbe implements Callback,Token,Context {
    public Object apply(Object value){Trace.callbackInput=value;Trace.step(4);return Trace.result;}
    public static final class Child extends ProtectedOverloadIdentifiersProbe {}
}
