package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.HashMap;
import java.util.Map;

public final class PairedContextSinksHarness {
    private static void check(boolean condition) {
        if(!condition)throw new AssertionError("owned paired context sinks mismatch");
    }
    private static final class Failure extends Exception {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value) throws T {throw (T)value;}
    private static final class Source extends Context {
        Observer observer;
        @Override public void callback(Map target,String key,Class first,Class second) {
            Observer o=observer;
            check(o.stage==2 && target==o.target && key==o.key && first==o.first && second==o.second && o.calls++==0);
            o.fail(9);
            if(o.mode==15)valueReply=o.replacement;
            if(o.mode==14)keyReply=o.alternate;
        }
    }
    private static final class Destination extends HashMap {
        Observer observer;
        @Override public Object put(Object key,Object value) {
            Observer o=observer;
            check(o.stage==3 && o.puts++==0 && key==o.capturedKey && value==o.capturedValue);
            o.fail(17);
            Object previous=super.put(key,value);
            o.applied=true;
            o.fail(18);
            return previous;
        }
    }
    private static final class Observer extends Trace {
        final int route,mode;
        final Object owner,target,key,first,second;
        final Source source;
        final Destination destination;
        final Throwable marker;
        final String replacement,alternate;
        int stage,calls,puts;
        boolean failed,applied;
        Object capturedKey,capturedValue;
        Observer(int route,int mode,Object owner,Object target,Object key,Object first,Object second,Source source,
                 Destination destination,Throwable marker,String replacement,String alternate) {
            this.route=route;this.mode=mode;this.owner=owner;this.target=target;this.key=key;this.first=first;this.second=second;
            this.source=source;this.destination=destination;this.marker=marker;this.replacement=replacement;this.alternate=alternate;
        }
        void fail(int wanted) {if(mode==wanted){failed=true;throw unchecked(marker);}}
        @Override public void observe(int phase,Context context,Map map,String name,Class a,Class b,Object left,Object right) {
            check(context==owner && map==target && name==key && a==first && b==second);
            if(phase==3) {check(stage==0 && left==null && right==null);stage=1;fail(8);return;}
            if(phase==1) {
                check(stage==1 && left==null && right==null);stage=2;
                if(mode==24)source.valueReply=replacement;
                return;
            }
            if(phase==2) {
                check(stage==2 && calls==1 && left==source.keyReply && right==source.valueReply);
                capturedKey=left;capturedValue=right;stage=3;
                if(mode==16){source.keyReply=alternate;source.valueReply=replacement;}
                fail(10);return;
            }
            if(phase==5) {
                check(stage==3 && puts==1 && applied && left==capturedKey && right==capturedValue && destination.get(left)==right);
                stage=4;fail(11);return;
            }
            check(phase==4 && stage==4 && left==null && right==null);stage=5;fail(23);
        }
        void finish(Object returned,Throwable problem) {
            if(mode>=4 && mode<=7 || mode==12)check(problem instanceof ClassCastException && stage==0);
            else if(mode==3)check(problem instanceof NullPointerException && stage==1 && calls==0);
            else if(mode==13)check(problem instanceof NullPointerException && stage==3 && puts==0);
            else if(mode==8 || mode==9 || mode==10 || mode==11 || mode==17 || mode==18 || mode==23)
                check(failed && problem==marker && returned==null);
            else if(mode==20 && route!=2)check(problem instanceof ClassCastException && stage==5);
            else check(problem==null && stage==5 && (route==2 ? returned==null : returned==destination.get(key)));
            if(!(mode>=4 && mode<=7) && mode!=12 && mode!=3 && mode!=8)check(calls==1);
            if(applied)check(destination.get(capturedKey)==capturedValue);
            if(mode==17)check(!applied && destination.isEmpty());
            if(mode==18)check(applied && puts==1);
            if(mode==16 && applied)check(capturedKey==key && capturedValue!=replacement);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        check(route>=0 && route<3 && mode>=0 && mode<25);
        for(int attempt=0;attempt<2;++attempt) {
            String key=new String("key"),replacement=new String("replacement"),alternate=new String("alternate");
            Source source=new Source();source.keyReply=key;source.valueReply=new String("initial");
            Destination destination=new Destination();
            Object owner=source,target=destination,name=key,first=String.class,second=Object.class;
            if(mode==1){name=null;source.keyReply=null;}
            if(mode==2)first=null;
            if(mode==3)owner=null;
            if(mode==4)owner=new Object();
            if(mode==5)name=new Object();
            if(mode==6)first=new Object();
            if(mode==7)second=new Object();
            if(mode==12)target=new Object();
            if(mode==13)target=null;
            if(mode==19)second=null;
            if(mode==20)source.valueReply=new Object();
            if(mode==21)source.valueReply="";
            if(mode==22)source.valueReply=null;
            Throwable marker=mode==10 || mode==11 || mode==18 || mode==23 ? new Fault() : new Failure();
            Observer observer=new Observer(route,mode,owner,target,name,first,second,source,destination,marker,replacement,alternate);
            source.observer=observer;destination.observer=observer;Trace.hook=observer;Object result=null;Throwable problem=null;
            try {
                if(route==0)result=Core.run(owner,target,name,first,second);
                else if(route==1)result=MethodHandles.lookup().findStatic(Core.class,"run",
                    MethodType.methodType(String.class,Object.class,Object.class,Object.class,Object.class,Object.class)).invokeWithArguments(owner,target,name,first,second);
                else Core.ignore(owner,target,name,first,second);
            } catch(Throwable failure) {problem=failure;}
            observer.finish(result,problem);check(Trace.hook==observer);Trace.hook=null;
        }
        System.out.println("attempts=2,paired-context-sinks=matched");
    }
}
