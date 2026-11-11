import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class BoundedPairCallbackHarness {
    static final long WIDE=0x8123456789abcdefL;
    static void check(boolean ok) {
        if(!ok)throw new AssertionError("owned bounded pair callback behavior");
    }
    static final class Probe extends Source<Object,Bound> {
        Probe(Object value,Bound vector,long tick,boolean running) { super(value,vector,tick,running); }
        public Object value() { Trace.hit(2);Trace.virtualCalls++;return value; }
    }
    static final class Callback implements Action<Scope<Object,Bound>,Unit> {
        public Unit apply(Scope<Object,Bound> scope) {
            check(scope==Trace.scope&&scope.source==Trace.source&&scope.state==Trace.state&&scope.tick==WIDE);
            Trace.calledScope=scope;Trace.hit(14);return Trace.returnedUnit;
        }
    }
    static final class Resume implements Completion<Unit> {
        public void resume(Unit unit) { check(unit==Trace.returnedUnit);Trace.calledUnit=unit;Trace.hit(15); }
    }
    static Object call(int route,Source source,State state,Action action,Completion completion) throws Throwable {
        if(route==0)return Client.call(source,state,WIDE,action,completion);
        if(route==1)return BoundedPairCallbackCaller.call(source,state,WIDE,action,completion);
        if(route==2) {
            try { return Provider.class.getMethod("consume",Source.class,State.class,long.class,Action.class,Completion.class)
                .invoke(null,source,state,WIDE,action,completion); }
            catch(InvocationTargetException e) { throw e.getCause(); }
        }
        if(route==3) {
            MethodHandle h=MethodHandles.lookup().findStatic(Provider.class,"consume",
                MethodType.methodType(Object.class,Source.class,State.class,long.class,Action.class,Completion.class));
            return (Object)h.invokeExact(source,state,WIDE,action,completion);
        }
        throw new AssertionError();
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++) {
            Object token=mode==3?null:new Object();Bound vector=mode==4?null:new Bound(31+attempt);
            long tick=WIDE+attempt;
            Source source=mode==1?null:new Probe(token,vector,tick,attempt==0);
            State state=mode==2?null:new State();
            Object old=new Object();Bound previous=new Bound(-1);
            if(state!=null) { state.value=old;state.vector=previous;state.tick=-7;state.running=false; }
            Action action=mode==18?null:new Callback();Completion completion=mode==19?null:new Resume();
            Trace.events="";Trace.failAt=mode>=5&&mode<=17?mode-4:mode==24?14:mode==25?15:0;Trace.virtualCalls=0;
            Trace.source=null;Trace.state=null;Trace.scope=null;Trace.calledScope=null;Trace.callback=null;Trace.calledUnit=null;
            Trace.returnedUnit=mode==20?null:mode==23?new Unit():Unit.INSTANCE;
            Trace.result=mode==21?null:mode==22?state:new Object();
            Trace.failure=new RuntimeException("owned failure");Object result=null;Throwable caught=null;
            try { result=call(route,source,state,action,completion); } catch(Throwable e) { caught=e; }
            String expected="";int reached=0;boolean called=false,resumed=false;
            int[] order={1,2,3,4,5,6,7,8,9,10,11,14,12,15,13};
            for(int event:order) {
                expected+=event+",";
                if(event<=11)reached=event;
                if(event==14)called=true;if(event==15)resumed=true;
                if(Trace.failAt==event || (mode==1&&event==1) || (mode==2&&event==2)
                        || (mode==18&&event==11) || (mode==19&&event==12))break;
            }
            check(Trace.events.equals(expected));check(Trace.source==source&&Trace.state==state);
            if(Trace.failAt!=0)check(caught==Trace.failure);
            else if(mode==1||mode==2||mode==18||mode==19)check(caught instanceof NullPointerException);
            else check(caught==null&&result==Trace.result);
            check(Trace.virtualCalls==((source!=null&&reached>=2&&Trace.failAt!=2)?1:0));
            if(state!=null) {
                check(state.value==((reached>=3&&Trace.failAt!=3)?token:old));
                check(state.vector==((reached>=5&&Trace.failAt!=5)?vector:previous));
                check(state.tick==((reached>=8&&Trace.failAt!=8)?tick:-7));
                check(state.running==(reached>=10&&Trace.failAt!=10&&attempt==0));
            }
            check((Trace.calledScope!=null)==called);
            check(Trace.callback==((called&&Trace.failAt!=14)?Trace.returnedUnit:null));
            check(Trace.calledUnit==(resumed?Trace.returnedUnit:null));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
