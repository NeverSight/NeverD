import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
public final class NestedCompletionResultHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned nested completion result");
    }
    static Object call(int route, Context context, Callback block, boolean initialized) throws Throwable {
        if (initialized) {
            if (route==0) return Initializer.value;
            if (route==1) return NestedCompletionResultCaller.initialized();
            if (route==2) return Initializer.class.getField("value").get(null);
            return (Object)MethodHandles.publicLookup().findStaticGetter(Initializer.class,"value",Object.class).invokeExact();
        }
        if (route==0) return Client.call(context,block);
        if (route==1) return NestedCompletionResultCaller.call(context,block);
        if (route==2) try { return Client.class.getMethod("call",Context.class,Callback.class).invoke(null,context,block); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
        return (Object)MethodHandles.publicLookup().findStatic(Client.class,"call",
                MethodType.methodType(Object.class,Context.class,Callback.class)).invokeExact(context,block);
    }
    static List<Integer> events(int mode, int fail, int attempt, boolean initialized) {
        List<Integer> result=new ArrayList<>();
        if (initialized&&attempt==1) return result;
        result.add(1); if (fail==1) return result;
        result.add(2); if (fail==2||attempt==1&&fail==3) return result;
        if (attempt==0) result.add(3); if (fail==3) return result;
        result.add(4); if (fail==4||mode==14) return result;
        result.add(5); if (fail==5||mode==15) return result;
        result.add(6); if (fail==6||mode==16||mode==17) return result;
        if (mode==1||mode==3||mode==4||fail>0) { result.add(7); if (fail==7) return result; }
        result.add(8); if (fail==8||mode==2||mode==3||mode==4) return result;
        result.add(9); return result;
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), mode=Integer.parseInt(args[1]);
        boolean initialized=Boolean.parseBoolean(args[2]);
        int fail=mode>=5&&mode<=13 ? mode-4 : 0;
        Context context=new Context(), lateContext=new Context();
        context.scope=new Scope(); Object marker=new Object(); context.scope.key=marker;
        InterruptedException interruption=new InterruptedException("owned interruption");
        if (mode==4) context.interruption=interruption;
        RuntimeException completedFailure=new IllegalStateException("owned completion failure");
        RuntimeException callbackFailure=new IllegalArgumentException("owned callback failure");
        Error callbackError=new AssertionError("owned callback error");
        Trace.failure=new IllegalArgumentException("owned controlled failure"); Trace.fail=fail;
        Object direct=new Object(), delivered=new Object();
        Callback<Scope,Completion<? super Object>,Object>[] self=new Callback[1];
        Callback lateBlock=(a,b)->{throw new AssertionError("owned late callback must not run");};
        self[0]=(scope,completion)->{
            require(scope==context.scope&&scope.key==marker&&completion==Trace.completion&&Trace.callback==self[0]);
            Trace.callbackCalls++; Trace.hit(6);
            if (mode==16) throw callbackFailure;
            if (mode==17) throw callbackError;
            Context.installed=lateContext; Context.block=lateBlock;
            if (mode==3) completion.failure=completedFailure;
            boolean complete=mode==1||mode==3||mode==4||fail>0;
            if (complete) completion.complete(delivered);
            return complete||mode==2 ? Context.SUSPENDED : direct;
        };
        Context argumentContext=mode==14 ? null : context;
        Callback argumentBlock=mode==15 ? null : self[0];
        for (int attempt=0;attempt<2;attempt++) {
            Trace.events=0; Trace.callbackCalls=0;
            Trace.contextArgument=null; Trace.callbackArgument=null; Trace.context=null; Trace.callback=null;
            Trace.completion=null; Trace.scope=null; Trace.returned=null; Trace.result=null;
            Context.installed=argumentContext; Context.block=argumentBlock;
            Object result=null; Throwable failure=null;
            try { result=call(route,argumentContext,argumentBlock,initialized); }
            catch (Throwable actual) { failure=actual; }
            List<Integer> expected=events(mode,fail,attempt,initialized);
            long trace=0; for (int event:expected) trace=trace*19+event;
            require(Trace.events==trace);
            require(Trace.contextArgument==(expected.contains(1)?argumentContext:null));
            require(Trace.callbackArgument==(expected.contains(2)?argumentBlock:null));
            require(Trace.context==(expected.contains(4)?argumentContext:null));
            require(Trace.callback==(expected.contains(4)?argumentBlock:null));
            boolean allocated=expected.contains(4)&&fail!=4;
            require((Trace.completion!=null)==allocated);
            require(Trace.scope==(expected.contains(5)?context.scope:null));
            require(Trace.callbackCalls==(expected.contains(6)?1:0));
            if (allocated) {
                Completion<?> completion=(Completion<?>)Trace.completion;
                require(completion.done==expected.contains(7)&&completion.value==(expected.contains(7)?delivered:null));
                require(completion.failure==(mode==3&&expected.contains(7)?completedFailure:null));
            }
            Object returned=mode==1||mode==2||mode==3||mode==4||fail>0 ? Context.SUSPENDED : direct;
            Object selected=mode==0 ? direct : delivered;
            require(Trace.returned==(expected.contains(8)?returned:null));
            require(Trace.result==(expected.contains(9)?selected:null));
            boolean success=mode==0||mode==1;
            if (initialized&&attempt==1&&!success) require(failure instanceof NoClassDefFoundError);
            else if (fail==3) {
                if (attempt==0) require(failure instanceof ExceptionInInitializerError&&failure.getCause()==Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (success) require(failure==null&&result==selected);
            else {
                Throwable cause=initialized&&mode!=17 ? failure==null?null:failure.getCause() : failure;
                if (initialized&&mode!=17) require(failure instanceof ExceptionInInitializerError);
                if (fail>0) require(cause==Trace.failure);
                else if (mode==2) require(cause instanceof IllegalStateException&&"owned missing completion".equals(cause.getMessage()));
                else if (mode==3) require(cause==completedFailure);
                else if (mode==4) require(cause==interruption);
                else if (mode==14||mode==15) require(cause instanceof NullPointerException);
                else if (mode==16) require(cause==callbackFailure);
                else require(cause==callbackError);
            }
            System.out.println("route="+route+",mode="+mode+",initializer="+initialized+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
