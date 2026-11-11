package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.HashMap;

public final class PairedContextTokensHarness {
    private static void check(boolean condition) {
        if(!condition)throw new AssertionError("owned paired context tokens mismatch");
    }
    private static final class Failure extends Exception {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value) throws T {throw (T)value;}
    private static final class Source extends Context {
        Observer observer;
        @Override public void callback(String key,Class first,Class second) {
            Observer o=observer;
            check(o.stage==2 && key==o.key && first==o.first && second==o.second && o.calls++==0);
            o.fail(9);
            if(o.mode==14)reply=o.later;
            if(o.mode==15)((HashMap)reply).put(key,o.replacement);
        }
    }
    private static final class Observer extends Trace {
        final int route,mode;
        final Object owner,key,first,second;
        final Source source;
        final Throwable marker;
        final String replacement;
        final HashMap later;
        int stage,calls;
        boolean failed;
        HashMap result;
        Observer(int route,int mode,Object owner,Object key,Object first,Object second,Source source,
                 Throwable marker,String replacement,HashMap later) {
            this.route=route;this.mode=mode;this.owner=owner;this.key=key;this.first=first;this.second=second;
            this.source=source;this.marker=marker;this.replacement=replacement;this.later=later;
        }
        void fail(int wanted) {if(mode==wanted){failed=true;throw unchecked(marker);}}
        @Override public void observe(int phase,Context context,String name,Class left,Class right,Object value) {
            check(context==owner && name==key && left==first && right==second);
            if(phase==3) {check(stage==0 && value==null);stage=1;fail(8);return;}
            if(phase==1) {
                check(stage==1 && value==null);stage=2;
                if(mode==24)source.reply=later;
                return;
            }
            if(phase==2) {
                check(stage==2 && calls==1 && value==source.reply);
                result=(HashMap)value;stage=3;
                if(mode==14 || mode==24)check(result==later);
                if(mode==23)check(result==null);
                if(mode==15)check(result.get(key)==replacement);
                if(mode==16)source.reply=later;
                if(mode==20)result.put(key,replacement);
                fail(10);return;
            }
            check(phase==4 && stage==3 && (route==2 ? value==null : value==result));
            stage=4;
            if(mode==17)source.reply=later;
            fail(11);
        }
        void finish(Object returned,Throwable problem) {
            if(mode>=4 && mode<=7)check(problem instanceof ClassCastException && stage==0);
            else if(mode==3)check(problem instanceof NullPointerException && stage==1 && calls==0);
            else if(mode>=8 && mode<=11)check(failed && problem==marker && returned==null);
            else if(mode==13)check(problem instanceof ClassCastException && stage==2 && calls==1);
            else if(mode==12 && route!=2)check(problem instanceof ClassCastException && stage==4);
            else if(mode==23 && route!=2)check(problem instanceof NullPointerException && stage==4);
            else check(problem==null && stage==4 && (route==2 ? returned==null : returned==result.get(key)));
            if(!(mode>=4 && mode<=7) && mode!=3 && mode!=8)check(calls==1);
            if(mode==16 || mode==17 || mode==24 || mode==14)check(source.reply==later);
            if(mode==20)check(result.get(key)==replacement);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        check(route>=0 && route<3 && mode>=0 && mode<25);
        for(int attempt=0;attempt<2;++attempt) {
            String key=new String("key"),replacement=new String("replacement");
            HashMap later=new HashMap();later.put(key,new String("later"));
            Source source=new Source();HashMap initial=new HashMap();initial.put(key,new String("initial"));source.reply=initial;
            Object owner=source,name=key,first=String.class,second=Object.class;
            if(mode==1){name=null;initial.put(null,new String("null-key"));later.put(null,new String("later-null"));}
            if(mode==2)first=null;
            if(mode==3)owner=null;
            if(mode==4)owner=new Object();
            if(mode==5)name=new Object();
            if(mode==6)first=new Object();
            if(mode==7)second=new Object();
            if(mode==12)initial.put(key,new Object());
            if(mode==13)source.reply=new Object();
            if(mode==18){first=Integer.class;second=CharSequence.class;}
            if(mode==19)second=null;
            if(mode==21)initial.put(key,"");
            if(mode==22)initial.put(key,null);
            if(mode==23)source.reply=null;
            Throwable marker=mode==10 || mode==11 ? new Fault() : new Failure();
            Observer observer=new Observer(route,mode,owner,name,first,second,source,marker,replacement,later);
            source.observer=observer;Trace.hook=observer;Object result=null;Throwable problem=null;
            try {
                if(route==0)result=Core.run(owner,name,first,second);
                else if(route==1)result=MethodHandles.lookup().findStatic(Core.class,"run",
                    MethodType.methodType(String.class,Object.class,Object.class,Object.class,Object.class)).invokeWithArguments(owner,name,first,second);
                else Core.ignore(owner,name,first,second);
            } catch(Throwable failure) {problem=failure;}
            observer.finish(result,problem);check(Trace.hook==observer);Trace.hook=null;
        }
        System.out.println("attempts=2,paired-context-tokens=matched");
    }
}
