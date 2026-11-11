import fixture.*;
import kotlin.jvm.functions.Function2;
import kotlin.coroutines.*;
public class ScopedOwnerCompletionProbe {
    public static class Block implements Function2 {
        public Object invoke(Object owner,Object completion) {
            Trace.callbackOwner=owner;Trace.completionSeen=completion;Trace.step(4);
            if(Trace.resume)((Continuation)completion).resumeWith(Trace.resumeValue);
            Trace.step(6);return Trace.result;
        }
    }
    public static final class Child extends Block {}
    public static final class Completion implements Continuation {
        public CoroutineContext getContext(){return EmptyCoroutineContext.INSTANCE;}
        public void resumeWith(Object value){Trace.resumedValue=value;Trace.step(5);}
    }
}
