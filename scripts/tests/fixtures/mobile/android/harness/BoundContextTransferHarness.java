package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.ArrayList;
import java.util.List;

public final class BoundContextTransferHarness {
    private static void check(boolean condition) {
        if(!condition)throw new AssertionError("owned bound context transfer mismatch");
    }
    private static final class Value implements Bound {}
    private static final class Failure extends Exception {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value) throws T {throw (T)value;}
    private static final class Source extends Context {
        Observer observer;
        @Override public void callback(List target,String key) {
            Observer o=observer;
            check(o.stage==2 && target==o.target && key==o.key && o.calls++==0);
            o.fail(9);
            if(o.mode==14)reply=o.later;
            if(o.mode==15)valueReply=o.replacement;
        }
    }
    private static final class Destination extends ArrayList {
        Observer observer;
        @Override public Object set(int index,Object value) {
            Observer o=observer;
            check(o.stage==3 && o.writes++==0 && index==0 && value==o.capturedValue);
            o.fail(18);
            Object previous=super.set(index,value);
            o.applied=true;o.fail(19);return previous;
        }
    }
    private static final class Observer extends Trace {
        final int route,mode;
        final Object owner,target,key;
        final Source source;
        final Destination destination;
        final Throwable marker;
        final Bound replacement;
        final List later;
        int stage,calls,writes;
        boolean failed,applied;
        Object capturedValue;
        List result;
        Observer(int route,int mode,Object owner,Object target,Object key,Source source,
                 Destination destination,Throwable marker,Bound replacement,List later) {
            this.route=route;this.mode=mode;this.owner=owner;this.target=target;this.key=key;
            this.source=source;this.destination=destination;this.marker=marker;this.replacement=replacement;this.later=later;
        }
        void fail(int wanted) {if(mode==wanted){failed=true;throw unchecked(marker);}}
        @Override public void observe(int phase,Context context,List list,String name,Object returned,Object value) {
            check(context==owner && list==target && name==key);
            if(phase==3) {check(stage==0 && returned==null && value==null);stage=1;fail(8);return;}
            if(phase==1) {
                check(stage==1 && returned==null && value==null);stage=2;
                if(mode==24){source.reply=later;source.valueReply=replacement;}
                return;
            }
            if(phase==2) {
                check(stage==2 && calls==1 && returned==source.reply && value==source.valueReply);
                result=(List)returned;capturedValue=value;stage=3;
                if(mode==16){source.reply=later;source.valueReply=replacement;}
                fail(10);return;
            }
            if(phase==5) {
                check(stage==3 && writes==1 && applied && returned==result && value==capturedValue && destination.get(0)==value);
                stage=4;fail(11);return;
            }
            check(phase==4 && stage==4 && (route==2 ? returned==null : returned==result) && value==null);stage=5;
            if(mode==17)source.reply=later;
        }
        void finish(Object returned,Throwable problem) {
            if(mode==4 || mode==5 || mode==7)check(problem instanceof ClassCastException && stage==0);
            else if(mode==3)check(problem instanceof NullPointerException && stage==1 && calls==0);
            else if(mode==6 || mode==12)check(problem instanceof ClassCastException && stage==2);
            else if(mode==13)check(problem instanceof NullPointerException && stage==3 && writes==0);
            else if(mode==8 || mode==9 || mode==10 || mode==11 || mode==18 || mode==19)
                check(failed && problem==marker && returned==null);
            else if(mode==20 && route!=2)check(problem instanceof ClassCastException && stage==5);
            else if(mode==23 && route!=2)check(problem instanceof NullPointerException && stage==5);
            else check(problem==null && stage==5 && (route==2 ? returned==null : returned==result.get(0)));
            if(mode!=4 && mode!=5 && mode!=7 && mode!=3 && mode!=8)check(calls==1);
            if(applied)check(destination.get(0)==capturedValue);
            if(mode==18)check(!applied && writes==1);
            if(mode==19)check(applied && writes==1);
            if(mode==16 && applied)check(capturedValue!=replacement);
            if(mode==14 || mode==16 || mode==17 || mode==24)check(source.reply==later);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        check(route>=0 && route<3 && mode>=0 && mode<25);
        for(int attempt=0;attempt<2;++attempt) {
            String key=new String("key");Bound replacement=new Value();
            Source source=new Source();source.valueReply=new Value();
            List initial=new ArrayList();initial.add(new Value());source.reply=initial;
            List later=new ArrayList();later.add(new Value());
            Destination destination=new Destination();destination.add(new Value());
            Object owner=source,target=destination,name=key;
            if(mode==1)name=null;
            if(mode==2)source.valueReply=null;
            if(mode==3)owner=null;
            if(mode==4)owner=new Object();
            if(mode==5)name=new Object();
            if(mode==6)source.valueReply=new Object();
            if(mode==7)target=new Object();
            if(mode==12)source.reply=new Object();
            if(mode==13)target=null;
            if(mode==20)initial.set(0,new Object());
            if(mode==21)source.reply=destination;
            if(mode==22)initial.set(0,null);
            if(mode==23)source.reply=null;
            Throwable marker=mode==10 || mode==11 || mode==19 ? new Fault() : new Failure();
            Observer observer=new Observer(route,mode,owner,target,name,source,destination,marker,replacement,later);
            source.observer=observer;destination.observer=observer;Trace.hook=observer;Object result=null;Throwable problem=null;
            try {
                if(route==0)result=Core.run(owner,target,name);
                else if(route==1)result=MethodHandles.lookup().findStatic(Core.class,"run",
                    MethodType.methodType(Bound.class,Object.class,Object.class,Object.class)).invokeWithArguments(owner,target,name);
                else Core.ignore(owner,target,name);
            } catch(Throwable failure) {problem=failure;}
            observer.finish(result,problem);check(Trace.hook==observer);Trace.hook=null;
        }
        System.out.println("attempts=2,bound-context-transfer=matched");
    }
}
