package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
@SuppressWarnings({"unchecked","rawtypes"})
public final class InterfaceTokenResultThrowsHarness {
    static final class Future extends Provider {
        final int mode; final Throwable fault;
        Future(int mode,Throwable fault){this.mode=mode;this.fault=fault;}
        @Override public <T> T fetch(Class<T> token) throws IOException {
            Trace.hit(2);Trace.overrides++;
            if(mode==8) return (T)Trace.value;
            if(mode==9) throw (IOException)fault;
            return super.fetch(token);
        }
    }
    static void check(boolean value){if(!value)throw new AssertionError("owned interface token result throws mismatch");}
    static <E extends Throwable> void raise(Throwable failure)throws E{throw (E)failure;}
    static Object invoke(int route,int operation,Core core,Class<?> token)throws Throwable {
        if(route==0)return operation==0?Core.call(core,token):Core.checked(core,token);
        if(route==1)return InterfaceTokenResultThrowsCaller.call(operation,core,token);
        String name=operation==0?"call":"checked";
        if(route==2){try{return Core.class.getMethod(name,Core.class,Class.class).invoke(null,core,token);}catch(InvocationTargetException e){throw e.getCause();}}
        return MethodHandles.lookup().findStatic(Core.class,name,MethodType.methodType(operation==0?Object.class:String.class,Core.class,Class.class)).invokeWithArguments(core,token);
    }
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Throwable fault=mode==13?new AssertionError("owned failure"):mode==12?new IllegalStateException("owned failure"):new IOException("owned failure");
        final IOException checked=new IOException("owned checked");
        final Provider receiver=mode==10?null:mode>=7&&mode<=9?new Future(mode,fault):new Provider();
        final Core core=mode==21?null:new Core(receiver);
        for(int attempt=0;attempt<2;attempt++) {
            if(core!=null)core.receiver=receiver;
            final Class<?> token=mode==1?null:mode==2?Integer.class:mode==22?int.class:mode==23?void.class:String.class;
            final Object initial=mode==3?null:mode==4?new Object():new String("owned value");
            final Object late=new String("owned late");final int[] counts={0,0,0};
            Trace.events=0;Trace.overrides=0;Trace.receiver=null;Trace.token=null;Trace.produced=null;Trace.result=null;Trace.value=initial;
            Trace.checked=mode==15||mode==18?checked:null;
            Trace.action=mode==11?null:new Runnable(){public void run(){
                counts[0]++;check(counts[0]==1&&Trace.receiver==receiver&&Trace.token==token&&Trace.events%16==3);
                if(mode==12||mode==13||mode==14)InterfaceTokenResultThrowsHarness.<RuntimeException>raise(fault);
                if(mode==5)Trace.value=late;
                if(mode==18)Trace.checked=null;
                if(mode==19)Trace.checked=checked;
                if(mode==20){core.receiver=null;Trace.token=null;}
            }};
            final Object expected=mode==5?late:initial;
            Trace.providerAfter=new Runnable(){public void run(){counts[1]++;check(counts[1]==1&&Trace.produced==expected&&Trace.events%16==5);if(mode==6)Trace.value=late;if(mode==16)InterfaceTokenResultThrowsHarness.<RuntimeException>raise(fault);}};
            Trace.after=new Runnable(){public void run(){counts[2]++;check(counts[2]==1&&Trace.result==expected&&Trace.events%16==7);if(mode==17)InterfaceTokenResultThrowsHarness.<RuntimeException>raise(fault);}};
            Object result=null;Throwable failure=null;
            try{result=invoke(route,operation,core,token);}catch(Throwable caught){failure=caught;}
            boolean bypass=mode==8||mode==9||mode==10||mode==21;
            boolean actionFailure=mode>=11&&mode<=14;
            boolean declared=mode==15||mode==19;
            boolean cast=operation==1&&expected!=null&&!(expected instanceof String);
            boolean produced=!bypass&&!actionFailure&&!declared;
            boolean providerSucceeded=mode==8||(produced&&mode!=16);
            boolean returned=providerSucceeded&&!cast;
            long events=1;
            if(mode>=7&&mode<=9)events=events*16+2;
            if(!bypass){events=events*16+3;if(!actionFailure){events=events*16+4;if(!declared){events=events*16+5;if(mode!=16)events=events*16+6;}}}
            if(returned){events=events*16+7;if(mode!=17)events=events*16+8;}
            check(Trace.events==events&&Trace.overrides==(mode>=7&&mode<=9?1:0));
            if(receiver!=null)check(receiver.calls==(bypass?0:attempt+1));
            check(counts[0]==(bypass||mode==11?0:1)&&counts[1]==(produced?1:0)&&counts[2]==(returned?1:0));
            check(Trace.receiver==(bypass?null:receiver)&&Trace.token==(bypass||mode==20?null:token));
            check(Trace.produced==(produced?expected:null)&&Trace.result==(returned?expected:null));
            if(mode==9||mode==12||mode==13||mode==14||mode==16||mode==17)check(result==null&&failure==fault);
            else if(declared)check(result==null&&failure==checked);
            else if(mode==10||mode==11||mode==21)check(result==null&&failure instanceof NullPointerException);
            else if(cast)check(result==null&&failure instanceof ClassCastException);
            else check(failure==null&&result==expected);
        }
        System.out.println("attempts=2,interface-token-result-throws=matched");
    }
}
