package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.ArrayList;
import java.util.List;

public final class UnaryTokenSinkHarness {
    private static void check(boolean condition) {
        if(!condition)throw new AssertionError("owned unary token sink mismatch");
    }
    private static final class Failure extends Exception {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value) throws T {throw (T)value;}
    private static final class Source extends Context {
        Observer observer;
        @Override public void callback(List target,String key,Class token) {
            Observer o=observer;
            check(o.stage==2 && target==o.target && key==o.key && token==o.token && o.calls++==0);
            o.fail(9);
            if(o.mode==14)valueReply=o.replacement;
            if(o.mode==24){((Destination)target).seed(o.replacement);o.seeded=true;}
        }
    }
    private static final class Destination extends ArrayList {
        Observer observer;
        void seed(Object value){super.set(0,value);}
        @Override public Object set(int index,Object value) {
            Observer o=observer;
            check(o.stage==3 && o.writes++==0 && index==0 && value==o.capturedValue);
            o.fail(17);
            Object previous=super.set(index,value);
            o.applied=true;o.fail(18);return previous;
        }
    }
    private static final class Observer extends Trace {
        final int route,mode;
        final Object owner,target,key,token;
        final Source source;
        final Destination destination;
        final Throwable marker;
        final String replacement;
        int stage,calls,writes;
        boolean failed,applied,seeded;
        Object capturedValue;
        Observer(int route,int mode,Object owner,Object target,Object key,Object token,Source source,
                 Destination destination,Throwable marker,String replacement) {
            this.route=route;this.mode=mode;this.owner=owner;this.target=target;this.key=key;this.token=token;
            this.source=source;this.destination=destination;this.marker=marker;this.replacement=replacement;
        }
        void fail(int wanted) {if(mode==wanted){failed=true;throw unchecked(marker);}}
        @Override public void observe(int phase,Context context,List list,String name,Class type,Object value) {
            check(context==owner && list==target && name==key && type==token);
            if(phase==3) {check(stage==0 && value==null);stage=1;fail(8);return;}
            if(phase==1) {
                check(stage==1 && value==null);stage=2;
                if(mode==15)source.valueReply=replacement;
                return;
            }
            if(phase==2) {
                check(stage==2 && calls==1 && value==source.valueReply);
                if(mode==24)check(seeded && destination.get(0)==replacement);
                capturedValue=value;stage=3;
                if(mode==16)source.valueReply=replacement;
                fail(10);return;
            }
            if(phase==5) {
                check(stage==3 && writes==1 && applied && value==capturedValue && destination.get(0)==value);
                stage=4;fail(11);return;
            }
            check(phase==4 && stage==4 && value==null);stage=5;fail(23);
        }
        void finish(Object returned,Throwable problem) {
            if(mode>=4 && mode<=7)check(problem instanceof ClassCastException && stage==0);
            else if(mode==3)check(problem instanceof NullPointerException && stage==1 && calls==0);
            else if(mode==13)check(problem instanceof NullPointerException && stage==3 && writes==0);
            else if(mode==8 || mode==9 || mode==10 || mode==11 || mode==17 || mode==18 || mode==23)
                check(failed && problem==marker && returned==null);
            else if((mode==12 || mode==20) && route!=2)check(problem instanceof ClassCastException && stage==5);
            else check(problem==null && stage==5 && (route==2 ? returned==null : returned==destination.get(0)));
            if(!(mode>=4 && mode<=7) && mode!=3 && mode!=8)check(calls==1);
            if(applied)check(destination.get(0)==capturedValue);
            if(mode==17)check(!applied && writes==1);
            if(mode==18)check(applied && writes==1);
            if(mode==16 && applied)check(capturedValue!=replacement);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        check(route>=0 && route<3 && mode>=0 && mode<25);
        for(int attempt=0;attempt<2;++attempt) {
            String key=new String("key"),replacement=new String("replacement");
            Source source=new Source();source.valueReply=new String("captured");
            Destination destination=new Destination();destination.add(new String("destination"));
            Object owner=source,target=destination,name=key,token=String.class;
            if(mode==1)name=null;
            if(mode==2)token=null;
            if(mode==3)owner=null;
            if(mode==4)owner=new Object();
            if(mode==5)name=new Object();
            if(mode==6)token=new Object();
            if(mode==7)target=new Object();
            if(mode==12)source.valueReply=new Object();
            if(mode==13)target=null;
            if(mode==19)token=Integer.class;
            if(mode==20)source.valueReply=new String[]{key};
            if(mode==21)source.valueReply="";
            if(mode==22)source.valueReply=null;
            Throwable marker=mode==10 || mode==11 || mode==18 || mode==23 ? new Fault() : new Failure();
            Observer observer=new Observer(route,mode,owner,target,name,token,source,destination,marker,replacement);
            source.observer=observer;destination.observer=observer;Trace.hook=observer;Object result=null;Throwable problem=null;
            try {
                if(route==0)result=Core.run(owner,target,name,token);
                else if(route==1)result=MethodHandles.lookup().findStatic(Core.class,"run",
                    MethodType.methodType(String.class,Object.class,Object.class,Object.class,Object.class)).invokeWithArguments(owner,target,name,token);
                else Core.ignore(owner,target,name,token);
            } catch(Throwable failure) {problem=failure;}
            observer.finish(result,problem);check(Trace.hook==observer);Trace.hook=null;
        }
        System.out.println("attempts=2,unary-token-sink=matched");
    }
}
