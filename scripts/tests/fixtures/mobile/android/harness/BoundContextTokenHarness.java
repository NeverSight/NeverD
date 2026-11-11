package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

public final class BoundContextTokenHarness {
    private static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned bound context token mismatch");
    }
    private static final class Failure extends Exception {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value) throws T {throw (T)value;}
    private static final class Source extends Context {
        Observer observer;
        @Override public void callback(String key,Class token) {
            Observer o=observer;
            check(o.stage==2 && key==o.key && token==o.token && o.calls++==0);
            o.fail(9);
            if(o.mode==14 || o.mode==23)reply=o.mode==23 ? null : o.later;
            if(o.mode==15)reply=o.replacement;
        }
    }
    private static final class Observer extends Trace {
        final int route,mode;
        final Object owner,key,token;
        final Source source;
        final Throwable marker;
        final String replacement;
        final String later;
        int stage,calls;
        boolean failed;
        Object result;
        Observer(int route,int mode,Object owner,Object key,Object token,Source source,
                 Throwable marker,String replacement,String later) {
            this.route=route;this.mode=mode;this.owner=owner;this.key=key;this.token=token;
            this.source=source;this.marker=marker;this.replacement=replacement;this.later=later;
        }
        void fail(int wanted) {if(mode==wanted){failed=true;throw unchecked(marker);}}
        @Override public void observe(int phase,Context context,String name,Class type,Object value) {
            check(context==owner && name==key && type==token);
            if(phase==3) {check(stage==0 && value==null);stage=1;fail(8);return;}
            if(phase==1) {
                check(stage==1 && value==null);stage=2;
                if(mode==24)source.reply=later;
                return;
            }
            if(phase==2) {
                check(stage==2 && calls==1 && value==source.reply);
                result=value;stage=3;
                if(mode==14 || mode==24)check(result==later);
                if(mode==23)check(result==null);
                if(mode==15)check(result==replacement);
                if(mode==16)source.reply=later;
                if(mode==20)((String[])result)[0]=replacement;
                fail(10);return;
            }
            check(phase==4 && stage==3 && (route==2 ? value==null : value==result));
            stage=4;
            if(mode==17)source.reply=later;
            fail(11);
        }
        void finish(Object returned,Throwable problem) {
            if(mode==4 || mode==5 || mode==6)check(problem instanceof ClassCastException && stage==0);
            else if(mode==3 || mode==19)check(problem instanceof NullPointerException && stage==2 && calls==0);
            else if(mode>=8 && mode<=11)check(failed && problem==marker && returned==null);
            else if((mode==12 || mode==13 || mode==20) && route!=2)check(problem instanceof ClassCastException && stage==4);
            else {
                check(problem==null && stage==4 && (route==2 ? returned==null : returned==result));
                if(mode==20)check(((String[])result)[0]==replacement);
            }
            if(mode!=4 && mode!=5 && mode!=6 && mode!=3 && mode!=19 && mode!=8)
                check(calls==1);
            if(mode==16 || mode==17 || mode==24 || mode==14)check(source.reply==later);
            if(mode==20)check(((String[])result)[0]==replacement);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        check(route>=0 && route<3 && mode>=0 && mode<25);
        for(int attempt=0;attempt<2;++attempt) {
            String a=new String("a"),b=new String("b"),key=new String("key"),replacement=new String("replacement");
            String later=new String("later");
            Source source=new Source();source.reply=a;
            Object owner=source,name=key,token=String.class;
            if(mode==1)name=null;
            if(mode==2 || mode==19)token=null;
            if(mode==3 || mode==19)owner=null;
            if(mode==4)owner=new Object();
            if(mode==5)name=new Object();
            if(mode==6)token=new Object();
            if(mode==7)source.reply=null;
            if(mode==12)source.reply=Integer.valueOf(7);
            if(mode==13)source.reply=new String[]{a,b};
            if(mode==18)token=Integer.class;
            if(mode==20)source.reply=new String[]{a,b};
            if(mode==21)source.reply=new String("");
            if(mode==22)source.reply=null;
            Throwable marker=mode==10 || mode==11 ? new Fault() : new Failure();
            Observer observer=new Observer(route,mode,owner,name,token,source,marker,replacement,later);
            source.observer=observer;Trace.hook=observer;Object result=null;Throwable problem=null;
            try {
                if(route==0)result=Core.run(owner,name,token);
                else if(route==1)result=MethodHandles.lookup().findStatic(Core.class,"run",
                    MethodType.methodType(String.class,Object.class,Object.class,Object.class)).invokeWithArguments(owner,name,token);
                else Core.ignore(owner,name,token);
            } catch(Throwable failure) {problem=failure;}
            observer.finish(result,problem);check(Trace.hook==observer);Trace.hook=null;
        }
        System.out.println("attempts=2,bound-context-token=matched");
    }
}
