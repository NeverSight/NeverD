import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class BoundedPairConsumerHarness {
    static void check(boolean ok) {
        if(!ok)throw new AssertionError("owned bounded pair consumer behavior");
    }
    static final class Probe extends Source<Object,Bound> {
        Probe(Object value,Bound vector,long tick,boolean running) { super(value,vector,tick,running); }
        public Object value() { Trace.hit(2);Trace.virtualCalls++;return value; }
    }
    static void call(int route,Source source,State state) throws Throwable {
        if(route==0)Client.call(source,state);
        else if(route==1)BoundedPairConsumerCaller.call(source,state);
        else if(route==2) {
            try { Provider.class.getMethod("consume",Source.class,State.class).invoke(null,source,state); }
            catch(InvocationTargetException e) { throw e.getCause(); }
        } else if(route==3) {
            MethodHandle h=MethodHandles.lookup().findStatic(Provider.class,"consume",
                MethodType.methodType(void.class,Source.class,State.class));
            h.invokeExact(source,state);
        } else throw new AssertionError();
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++) {
            Object token=mode==3?null:new Object();Bound vector=mode==4?null:new Bound(31+attempt);
            long tick=0x8123456789abcdefL+attempt;
            Source source=mode==1?null:new Probe(token,vector,tick,attempt==0);
            State state=mode==2?null:new State();
            Object old=new Object();Bound previous=new Bound(-1);
            if(state!=null) { state.value=old;state.vector=previous;state.tick=-7;state.running=false; }
            Trace.events="";Trace.failAt=mode>=5?mode-4:0;Trace.virtualCalls=0;
            Trace.source=null;Trace.state=null;Trace.failure=new RuntimeException("owned failure");
            Throwable caught=null;
            try { call(route,source,state); } catch(Throwable e) { caught=e; }
            String expected="";
            for(int event=1;event<=11;event++) {
                expected+=event+",";
                if(Trace.failAt==event || (mode==1&&event==1) || (mode==2&&event==2))break;
            }
            check(Trace.events.equals(expected));check(Trace.source==source&&Trace.state==state);
            if(Trace.failAt!=0)check(caught==Trace.failure);
            else if(mode==1||mode==2)check(caught instanceof NullPointerException);
            else check(caught==null);
            int reached=expected.equals("")?0:Integer.parseInt(expected.split(",")[expected.split(",").length-1]);
            check(Trace.virtualCalls==((source!=null&&reached>=2&&Trace.failAt!=2)?1:0));
            if(state!=null) {
                check(state.value==((reached>=3&&Trace.failAt!=3)?token:old));
                check(state.vector==((reached>=5&&Trace.failAt!=5)?vector:previous));
                check(state.tick==((reached>=8&&Trace.failAt!=8)?tick:-7));
                check(state.running==(reached>=10&&Trace.failAt!=10&&attempt==0));
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
