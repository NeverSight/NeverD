package fixture;
public final class Trace {
    private Trace() {}
    public static long events;
    public static int fail,calledKind;
    public static boolean run,complete;
    public static Throwable failure;
    public static Object blockSeen,receiverSeen,completionSeen,callbackReceiver,callbackCompletion,delivered,answer,callbackAnswer,payload;
    public static Completion result,callbackReplacement,finalReplacement;
    public static void step(int value) {
        events=events*10+value;
        if(value==6)result=finalReplacement;
        if(fail==value) {
            if(failure instanceof RuntimeException)throw (RuntimeException)failure;
            throw (Error)failure;
        }
    }
    public static Completion transfer(Object block,Object receiver,Completion completion,int kind) {
        blockSeen=block;receiverSeen=receiver;completionSeen=completion;step(1);step(2);
        if(run) {
            Object value=kind==0?((Unary)block).call(completion):((Binary)block).call(receiver,completion);
            callbackAnswer=value;
        }
        step(5);Completion value=result;step(6);return value;
    }
}
