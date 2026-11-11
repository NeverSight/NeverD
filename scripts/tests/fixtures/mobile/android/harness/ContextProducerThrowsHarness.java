package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.concurrent.Callable;

public final class ContextProducerThrowsHarness {
    private static void check(boolean condition) {
        if(!condition)throw new AssertionError("owned context producer throws mismatch");
    }
    private static final class Failure extends InterruptedException {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value) throws T {throw (T)value;}
    private static final class Producer implements Callable<Object> {
        Observer observer;
        Object reply;
        @Override public Object call() throws Exception {
            Observer o=observer;
            check(o.stage==2 && o.contextCalls==1 && o.producerCalls++==0);
            o.fail(9);
            if(o.mode==14)o.source.reply=o.later;
            return reply;
        }
    }
    private static final class Source extends Context {
        Observer observer;
        @Override public Object produce(Callable producer,int limit) throws InterruptedException {
            Observer o=observer;
            check(o.stage==2 && producer==o.producer && limit==o.limit && o.contextCalls++==0);
            o.fail(23);
            try {producer.call();}
            catch(InterruptedException value){throw value;}
            catch(RuntimeException value){throw value;}
            catch(Exception value){throw unchecked(value);}
            Object value=super.produce(producer,limit);
            o.snapshot=value;
            if(o.mode==16)reply=o.later;
            return value;
        }
    }
    private static final class Observer extends Trace {
        final int route,mode,limit;
        final Object owner,producer;
        final Source source;
        final Throwable marker;
        final String later;
        int stage,contextCalls,producerCalls;
        boolean failed;
        Object snapshot;
        Observer(int route,int mode,int limit,Object owner,Object producer,Source source,Throwable marker,String later) {
            this.route=route;this.mode=mode;this.limit=limit;this.owner=owner;this.producer=producer;
            this.source=source;this.marker=marker;this.later=later;
        }
        void fail(int wanted){if(mode==wanted){failed=true;throw unchecked(marker);}}
        @Override public void observe(int phase,Context context,Callable callback,int value,Object result) {
            check(context==owner && callback==producer && value==limit);
            if(phase==3){check(stage==0 && result==null);stage=1;fail(8);return;}
            if(phase==1){check(stage==1 && result==null);stage=2;if(mode==15)source.reply=later;return;}
            if(phase==5){check(stage==2 && contextCalls==1 && producerCalls==1 && result==null);stage=3;if(mode==24)source.reply=later;fail(10);return;}
            if(phase==2){
                check(stage==3 && contextCalls==1 && producerCalls==1 && result==snapshot);stage=4;
                if(mode==14 || mode==15 || mode==24)check(result==later);
                if(mode==17)source.reply=later;
                fail(11);return;
            }
            check(phase==4 && stage==4 && (route==2 ? result==null : result==snapshot));stage=5;fail(12);
        }
        void finish(Object returned,Throwable problem) {
            if(mode==4 || mode==5)check(problem instanceof ClassCastException && stage==0);
            else if(mode==3)check(problem instanceof NullPointerException && stage==2 && contextCalls==0 && producerCalls==0);
            else if(mode==2)check(problem instanceof NullPointerException && stage==2 && contextCalls==1 && producerCalls==0);
            else if(mode==8 || mode==9 || mode==10 || mode==11 || mode==12 || mode==23)check(failed && problem==marker && returned==null);
            else if((mode==19 || mode==20) && route!=2)check(problem instanceof ClassCastException && stage==5);
            else check(problem==null && stage==5 && (route==2 ? returned==null : returned==snapshot));
            if(mode!=4 && mode!=5 && mode!=3 && mode!=8)check(contextCalls==1);
            if(mode!=4 && mode!=5 && mode!=3 && mode!=2 && mode!=8 && mode!=23)check(producerCalls==1);
            if(mode==14 || mode==15 || mode==16 || mode==17 || mode==24)check(source.reply==later);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        check(route>=0 && route<3 && mode>=0 && mode<25);
        for(int attempt=0;attempt<2;++attempt) {
            Source source=new Source();source.reply=new String("result");
            Producer callback=new Producer();callback.reply=new String("callback");
            Object owner=source,producer=callback;int limit=17;
            if(mode==1)limit=-7;
            if(mode==2)producer=null;
            if(mode==3)owner=null;
            if(mode==4)owner=new Object();
            if(mode==5)producer=new Object();
            if(mode==6)limit=Integer.MAX_VALUE;
            if(mode==7)limit=Integer.MIN_VALUE;
            if(mode==13)source.reply=null;
            if(mode==18)callback.reply=new Object();
            if(mode==19)source.reply=new Object();
            if(mode==20)source.reply=new String[]{"value"};
            if(mode==21)source.reply="";
            if(mode==22)callback.reply=null;
            Throwable marker=mode==10 || mode==12 ? new Fault() : new Failure();
            String later=new String("later");Observer observer=new Observer(route,mode,limit,owner,producer,source,marker,later);
            source.observer=observer;callback.observer=observer;Trace.hook=observer;Object result=null;Throwable problem=null;
            try {
                if(route==0)result=Core.run(owner,producer,limit);
                else if(route==1)result=MethodHandles.lookup().findStatic(Core.class,"run",
                    MethodType.methodType(String.class,Object.class,Object.class,int.class)).invokeWithArguments(owner,producer,limit);
                else Core.ignore(owner,producer,limit);
            }catch(Throwable value){problem=value;}
            observer.finish(result,problem);check(Trace.hook==observer);Trace.hook=null;
        }
        System.out.println("attempts=2,context-producer-throws=matched");
    }
}
