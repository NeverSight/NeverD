import fixture.*;
import kotlin.jvm.functions.Function2;
import kotlin.coroutines.*;
public class ScopedSuspendForwarderProbe implements Scope {
    public Object token(){Trace.step(1);return Trace.contextValue;}
    public static final class Child extends ScopedSuspendForwarderProbe {}
    public static final class Block implements Function2 {
        public Object invoke(Object owner,Object completion) {
            Trace.callbackOwner=owner;Trace.completionSeen=completion;Trace.step(4);
            if(Trace.resume)((Continuation)completion).resumeWith(Trace.resumeValue);
            Trace.step(6);return Trace.result;
        }
    }
    public static final class Completion implements Continuation {
        public CoroutineContext getContext(){return EmptyCoroutineContext.INSTANCE;}
        public void resumeWith(Object value){Trace.resumedValue=value;Trace.step(5);}
    }
}
