import fixture.*;
public class FixedCompletionFactoryProbe implements Unary,Binary,Completion {
    private Object record(Object receiver,Object completion) {
        Trace.callbackReceiver=receiver;Trace.callbackCompletion=completion;
        Trace.result=Trace.callbackReplacement;Trace.step(3);
        if(Trace.complete)((Completion)completion).deliver(Trace.payload);
        return Trace.answer;
    }
    public Object call(Object completion){Trace.calledKind=1;return record(null,completion);}
    public Object call(Object receiver,Object completion){Trace.calledKind=2;return record(receiver,completion);}
    public void deliver(Object value){Trace.delivered=value;Trace.step(4);}
    public static final class Child extends FixedCompletionFactoryProbe {}
}
