package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
@SuppressWarnings({"unchecked","rawtypes"})
public final class InstanceProducerVarargsHarness {
    static final class Plain extends Provider {}
    static final class Derived extends Provider {
        final int mode; final Throwable fault;
        Derived(int mode,Throwable fault) { this.mode=mode; this.fault=fault; }
        @Override public <E> E read(Context context,Producer<E> producer,String... labels) throws IOException {
            Trace.hit(2); Trace.overrides++;
            if(mode==8) return (E)Trace.value;
            if(mode==9) throw (IOException)fault;
            return super.read(context,producer,labels);
        }
    }
    static void check(boolean value) { if(!value) throw new AssertionError("owned instance producer varargs mismatch"); }
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E)failure; }
    static Object invoke(int route,int operation,Provider receiver,String[] labels,Context context,Producer<?> producer) throws Throwable {
        if(route==0) return operation==0 ? Core.call(receiver,labels,context,producer) : Core.checked(receiver,labels,context,producer);
        if(route==1) return InstanceProducerVarargsCaller.call(operation,receiver,labels,context,producer);
        String name=operation==0 ? "call" : "checked";
        if(route==2) {
            try { return Core.class.getMethod(name,Provider.class,String[].class,Context.class,Producer.class).invoke(null,receiver,labels,context,producer); }
            catch(InvocationTargetException e) { throw e.getCause(); }
        }
        return MethodHandles.lookup().findStatic(Core.class,name,MethodType.methodType(operation==0 ? Object.class : String.class,Provider.class,String[].class,Context.class,Producer.class)).invokeWithArguments(receiver,labels,context,producer);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Throwable fault=mode==13||mode==19 ? new AssertionError("owned failure") : mode==12||mode==18 ? new IllegalStateException("owned failure") : new IOException("owned failure");
        final IOException checked=new IOException("owned checked");
        final Provider receiver=mode==10 ? null : mode>=7&&mode<=9 ? new Derived(mode,fault) : new Plain();
        for(int attempt=0;attempt<2;++attempt) {
            final String first=new String("owned first"),second=new String("owned second");
            final String replacement=new String("owned replacement"),callbackLabel=new String("owned callback"),lateLabel=new String("owned late label");
            final String[] labels=mode==1 ? null : mode==4 ? new String[0] : new String[]{mode==5 ? null : first,second};
            final Context context=mode==2 ? null : new Context();
            final Object initial=mode==4 ? null : mode==6 ? new Object() : new String("owned value");
            final Object late=new String("owned late"); final int[] counts={0,0};
            final Producer<Object> producer=mode==3 ? null : new Producer<Object>() { public Object produce(Context actual) {
                counts[1]++; check(counts[1]==1 && actual==context && Trace.context==context && Trace.labels==labels && Trace.producer==this && Trace.events%16==4);
                if(mode==18||mode==19||mode==20) InstanceProducerVarargsHarness.<RuntimeException>raise(fault);
                return Trace.value;
            }};
            Trace.beforeLabel=null; Trace.replacement=replacement;
            Trace.events=0; Trace.overrides=0; Trace.labels=null; Trace.context=null; Trace.producer=null; Trace.produced=null; Trace.result=null;
            Trace.inputLabels=labels; Trace.inputContext=context; Trace.inputProducer=producer; Trace.value=initial;
            if(context!=null) {
                context.checked=mode==15||mode==17 ? checked : null;
                context.action=mode==11 ? null : new Runnable() { public void run() {
                    counts[0]++; check(counts[0]==1 && Trace.labels==labels && Trace.context==context && Trace.producer==producer);
                    if(mode==12||mode==13||mode==14) InstanceProducerVarargsHarness.<RuntimeException>raise(fault);
                    if(mode==16) context.checked=checked;
                    if(mode==17) context.checked=null;
                    if(mode==23) { Trace.inputLabels=new String[]{lateLabel}; labels[0]=callbackLabel; Trace.inputContext=new Context(); Trace.inputProducer=null; Trace.value=late; }
                }};
            }
            final Object expected=mode==23 ? late : initial;
            Trace.providerAfter=new Runnable() { public void run() {
                check(Trace.produced==expected);
                if(mode==5) { Trace.value=late; labels[0]=lateLabel; }
                if(mode==21) InstanceProducerVarargsHarness.<RuntimeException>raise(fault);
            }};
            Trace.after=new Runnable() { public void run() {
                check(Trace.result==expected);
                if(mode==22) InstanceProducerVarargsHarness.<RuntimeException>raise(fault);
            }};
            Object result=null; Throwable failure=null;
            try {result=invoke(route,operation,receiver,labels,context,producer);} catch(Throwable caught) {failure=caught;}
            boolean bypass=mode==8||mode==9||mode==10;
            boolean actionFailure=mode==2||mode==11||mode==12||mode==13||mode==14;
            boolean declared=mode==15||mode==16;
            boolean producerFailure=mode==3||mode==18||mode==19||mode==20;
            boolean cast=operation==1 && expected!=null && !(expected instanceof String);
            long events=1;
            if(mode>=7&&mode<=9) events=events*16+2;
            if(!bypass) {
                events=events*16+3;
                if(!actionFailure) {
                    events=events*16+4;
                    if(!declared&&!producerFailure) { events=events*16+5; if(mode!=21) events=events*16+6; }
                }
            }
            boolean providerSucceeded=mode==8||(!bypass&&!actionFailure&&!declared&&!producerFailure&&mode!=21);
            if(providerSucceeded&&!cast) { events=events*16+7; if(mode!=22) events=events*16+8; }
            check(Trace.events==events && Trace.overrides==((mode>=7&&mode<=9)?1:0));
            if(receiver!=null) check(receiver.calls==((mode==8||mode==9)?0:attempt+1));
            check(counts[0]==(bypass||mode==2||mode==11?0:1));
            check(counts[1]==(bypass||actionFailure||declared||mode==3?0:1));
            if(bypass) check(Trace.labels==null && Trace.context==null && Trace.producer==null);
            else check(Trace.labels==labels && Trace.context==context && Trace.producer==producer);
            check(Trace.produced==(!bypass&&!actionFailure&&!declared&&!producerFailure?expected:null));
            check(Trace.result==(providerSucceeded&&!cast?expected:null));
            boolean touched=!bypass&&!actionFailure&&labels!=null&&labels.length!=0;
            String before=mode==23 ? callbackLabel : mode==5 ? null : first;
            check(Trace.beforeLabel==(touched?before:null));
            if(labels!=null&&labels.length!=0) {
                check(labels[0]==(touched?(mode==5?lateLabel:replacement):(mode==5?null:first)));
                check(labels[1]==second);
            }
            if(mode==9||mode==12||mode==13||mode==14||mode==18||mode==19||mode==20||mode==21||mode==22) check(result==null && failure==fault);
            else if(declared) check(result==null && failure==checked);
            else if(mode==2||mode==3||mode==10||mode==11) check(result==null && failure instanceof NullPointerException);
            else if(cast) check(result==null && failure instanceof ClassCastException);
            else check(failure==null && result==expected);
        }
        System.out.println("attempts=2,instance-producer-varargs=matched");
    }
}
