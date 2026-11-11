package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.function.Predicate;

public final class FluentTokenConsumerHarness {
    private static void check(boolean condition) {
        if(!condition)throw new AssertionError("owned fluent token consumer mismatch");
    }
    private static final class Failure extends Exception {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value) throws T {throw (T)value;}
    private static final class Source extends Provider {
        Observer observer;
        @Override public <T> Provider bind(String key,Class<T> token,Predicate<T> predicate) {
            Observer o=observer;check(o.overrides++==0);
            Provider result=super.bind(key,token,predicate);
            if(o.mode==21){Trace.returned=o.later;return o.overrideResult;}
            return result;
        }
    }
    private static final class Sink implements Predicate<String> {
        Observer observer;
        @Override public boolean test(String value) {
            Observer o=observer;check(o.stage==2 && o.calls++==0 && value==o.expectedValue);
            o.fail(9);
            if(o.mode==14)Trace.returned=o.later;
            if(o.mode==20){Trace.value=o.replacement;o.failed=true;throw unchecked(o.marker);}
            return o.wanted;
        }
    }
    private static final class Observer extends Trace {
        final int route,mode;
        final Object owner,key,token,predicate;
        final Throwable marker;
        final String replacement;
        final Provider later,overrideResult;
        final boolean wanted;
        int stage,calls,overrides;
        boolean failed;
        Object expectedValue;
        Provider result;
        Observer(int route,int mode,Object owner,Object key,Object token,Object predicate,
                 Throwable marker,String replacement,Provider later,Provider overrideResult,Object value) {
            this.route=route;this.mode=mode;this.owner=owner;this.key=key;this.token=token;
            this.predicate=predicate;this.marker=marker;this.replacement=replacement;
            this.later=later;this.overrideResult=overrideResult;expectedValue=value;wanted=mode!=1;
        }
        void fail(int wantedMode){if(mode==wantedMode){failed=true;throw unchecked(marker);}}
        @Override public void observe(int phase,Provider self,String name,Class type,Object callback,Object value) {
            check(self==owner && name==key && type==token && callback==predicate);
            if(phase==3){check(stage==0 && value==null);stage=1;fail(8);return;}
            if(phase==1){
                check(stage==1 && value==null && overrides==1);stage=2;
                if(mode==15){Trace.value=replacement;expectedValue=replacement;}
                return;
            }
            if(phase==2){
                check(stage==2 && calls==1 && value==Trace.returned && Trace.accepted==wanted);
                result=(Provider)value;stage=3;
                if(mode==14)check(result==later);
                if(mode==16)Trace.returned=later;
                fail(10);return;
            }
            Provider expected=mode==21 ? overrideResult : result;
            check(phase==4 && stage==3 && (route==2 ? value==null : value==expected));
            stage=4;if(mode==17)Trace.returned=later;fail(11);
        }
        void finish(Object returned,Throwable problem) {
            if(mode>=4 && mode<=7)check(problem instanceof ClassCastException && stage==0 && overrides==0);
            else if(mode==3)check(problem instanceof NullPointerException && stage==1 && overrides==0);
            else if(mode==12 || mode==24)check(problem instanceof ClassCastException && stage==2 && calls==0);
            else if(mode==13)check(problem instanceof NullPointerException && stage==2 && calls==0);
            else if((mode>=8 && mode<=11) || mode==20)check(failed && problem==marker && returned==null);
            else {
                Provider expected=mode==21 ? overrideResult : result;
                check(problem==null && stage==4 && (route==2 ? returned==null : returned==expected));
            }
            if(stage>=2)check(overrides==1);
            if(stage>=3)check(calls==1 && Trace.accepted==wanted);
            if(mode==14 || mode==16 || mode==17 || mode==21)check(Trace.returned==later);
            if(mode==15 || mode==20)check(Trace.value==replacement);
        }
    }
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        check(route>=0 && route<3 && mode>=0 && mode<25);
        for(int attempt=0;attempt<2;++attempt){
            Source source=new Source();Sink sink=new Sink();
            String key=new String("key"),value=new String("value"),replacement=new String("replacement");
            Provider ordinary=new Provider(),later=new Provider(),overrideResult=new Provider();
            Object owner=source,name=key,token=String.class,callback=sink;
            if(mode==3)owner=null;
            if(mode==4)owner=new Object();
            if(mode==5)name=new Object();
            if(mode==6)token=new Object();
            if(mode==7)callback=new Object();
            if(mode==13)callback=null;
            if(mode==18)token=Integer.class;
            if(mode==23)token=null;
            Trace.value=mode==12 ? Integer.valueOf(7) : mode==19 ? null : mode==24 ? new String[]{value} : value;
            Trace.returned=mode==2 ? null : mode==22 ? source : ordinary;
            Trace.accepted=mode==1;
            Throwable marker=mode==10 || mode==11 ? new Fault() : new Failure();
            Observer observer=new Observer(route,mode,owner,name,token,callback,marker,replacement,later,overrideResult,Trace.value);
            source.observer=observer;sink.observer=observer;Trace.hook=observer;
            Object result=null;Throwable problem=null;
            try{
                if(route==0)result=Core.run(owner,name,token,callback);
                else if(route==1)result=MethodHandles.lookup().findStatic(Core.class,"run",
                    MethodType.methodType(Provider.class,Object.class,Object.class,Object.class,Object.class)).invokeWithArguments(owner,name,token,callback);
                else Core.ignore(owner,name,token,callback);
            }catch(Throwable failure){problem=failure;}
            observer.finish(result,problem);check(Trace.hook==observer);Trace.hook=null;
        }
        System.out.println("attempts=2,fluent-token-consumer=matched");
    }
}
