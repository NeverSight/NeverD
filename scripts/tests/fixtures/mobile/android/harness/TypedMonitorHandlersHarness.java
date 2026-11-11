import fixture.*;
import java.lang.invoke.*;

public final class TypedMonitorHandlersHarness {
    static void check(boolean ok) {
        if(!ok)throw new AssertionError("owned typed monitor handlers mismatch");
    }
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        MethodHandle call=MethodHandles.lookup().findStatic(Core.class,"call",
            MethodType.methodType(Object.class,Object.class,Callback.class,Object.class));
        for(int attempt=0;attempt<2;++attempt) {
            Observer o=new Observer(route,mode);
            Object result=null;Throwable caught=null;
            try {
                if(route==0)result=Core.call(o.lock,o.callback,o.seed);
                else if(route==1)result=Core.nested(o.outer,o.lock,o.callback,o.seed);
                else result=(Object)call.invokeExact((Object)o.lock,(Callback)o.callback,(Object)o.seed);
            }catch(Throwable failure){caught=failure;}
            if(mode==1 || mode==3 || mode==2 && route==1)
                check(caught!=null && caught.getClass()==NullPointerException.class);
            else check(caught==o.failure());
            check(o.events.equals(o.expectedEvents()));
            if(caught==null)check(result==o.expectedResult());
            if(o.lock!=null)check(!Thread.holdsLock(o.lock));
            if(o.outer!=null)check(!Thread.holdsLock(o.outer));
        }
        System.out.println("attempts=2,typed-monitor-handlers=matched");
    }
    public static final class Observer implements Callback {
        final int route,mode;
        final Object lock,outer,seed=new Object(),offered=new Object(),changed=new Object();
        final Callback callback;
        final Specific specific=new Specific();
        final Problem problem=new Problem();
        final Exception checked=new Exception("owned checked failure");
        final Error error=new LinkageError("owned error failure");
        Object next;
        String events="";
        Observer(int route,int mode) {
            this.route=route;this.mode=mode;
            lock=mode==1?null:new Object();
            outer=mode==2?null:mode==19?lock:new Object();
            callback=mode==3?null:this;next=offered;
        }
        void held(Object a,Object value) {
            check(a==lock && value==seed && Thread.holdsLock(lock));
            if(outer!=null)check(Thread.holdsLock(outer)==(route==1 || outer==lock));
        }
        @Override public Object produce(Object a,Object value)throws Exception {
            held(a,value);events+="P,";
            if(mode==4 || mode==8 || mode==14 || mode==16 || mode==18)throw specific;
            if(mode==5 || mode==9 || mode==17)throw problem;
            if(mode==6 || mode==10)throw checked;
            if(mode==7 || mode==13)throw error;
            return next;
        }
        @Override public Object refine(Object a,Object value,Specific failure)throws Exception {
            held(a,value);events+="S,";check(failure==specific);
            if(mode==8)throw checked;
            return mode==16?null:next;
        }
        @Override public Object recover(Object a,Object value,Problem failure)throws Exception {
            held(a,value);events+="R,";check(failure==problem);
            if(mode==9)throw error;
            Object result=next;
            if(mode==17)next=changed;
            return result;
        }
        @Override public void observe(Object a,Object value,Exception failure)throws Exception {
            held(a,value);events+="O,";check(failure==checked);
            if(mode==10)throw problem;
        }
        @Override public void finish(Object a,Object value)throws Exception {
            held(a,value);events+="F,";
            if(mode==11)throw specific;
            if(mode==12)throw error;
            if(mode==13)throw problem;
            if(mode==14)throw checked;
            if(mode==15 || mode==18)next=changed;
        }
        Throwable failure() {
            if(mode==6 || mode==8 || mode==14)return checked;
            if(mode==7 || mode==9 || mode==12)return error;
            if(mode==10 || mode==13)return problem;
            return mode==11?specific:null;
        }
        Object expectedResult(){return mode==16?null:offered;}
        String expectedEvents() {
            if(mode==1 || mode==3 || mode==2 && route==1)return "";
            if(mode==4 || mode==8 || mode==14 || mode==16 || mode==18)return "P,S,F,";
            if(mode==5 || mode==9 || mode==17)return "P,R,F,";
            if(mode==6 || mode==10)return "P,O,F,";
            return "P,F,";
        }
    }
}
