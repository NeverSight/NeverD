import fixture.*;
import java.io.IOException;
import java.util.function.Function;
public final class ClassThrowableContextHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned class throwable context");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static final class FutureBox extends Box<String> {
        @Override public <Y extends Throwable> String get(Function<String,? extends Y> factory,String message)throws Y {Trace.overrides++;return super.get(factory,message);}
    }
    private static Object call(int route,Box<String> box,Function<String,RuntimeException> factory,String message){
        if(route==0)return Core.call(box,factory,message);
        if(route==1)return ClassThrowableContextCaller.generic(box,factory,message);
        if(route==2)return Core.checked(box,factory,message,false);
        return ClassThrowableContextCaller.typed(box,factory,message);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Box<String> box=mode==1?null:operation==0?new Box<String>():new FutureBox();
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object(),desiredValue=mode==2?null:mode==7?Integer.valueOf(7):new String("owned-value"),wrong=new Object();
            final String message=attempt==0?new String("owned-message"):null;final boolean present=mode==0||mode==1||mode==2||mode==7||mode==8;
            Trace.events=0;Trace.receiver=null;Trace.seenFactory=null;Trace.seenMessage=null;Trace.thrown=null;Trace.present=!present;Trace.value=sentinel;Trace.payload=sentinel;Trace.message=message;
            final int actions=Trace.actions,callbacks=Trace.callbacks,overrides=Trace.overrides;
            final Function raw=new Function(){public Object apply(Object context){Trace.callbacks++;require(Trace.events==12&&context==message&&Trace.receiver==box&&Trace.payload==payload);if(mode==6)throw error;if(mode==11)ClassThrowableContextHarness.<RuntimeException>fail(checked);return mode==4?null:mode==5?wrong:mode==9?checked:mode==10?error:runtime;}};
            final Function<String,RuntimeException> factory=(Function<String,RuntimeException>)raw;Trace.factory=factory;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==1&&Trace.receiver==box&&Trace.seenFactory==factory&&Trace.seenMessage==message&&Trace.payload==sentinel);Trace.present=present;Trace.value=desiredValue;Trace.payload=payload;if(mode==8)throw runtime;}};
            Object result=sentinel;String value=null;Throwable failure=null;try{result=call(route,box,factory,message);value=(String)result;require(value.length()==11);}catch(Throwable caught){failure=caught;}
            final long events=mode==1?0:mode==8?1:present?13:mode==5||mode==6||mode==11?12:124;
            require(Trace.events==events&&Trace.actions==actions+(mode==1?0:1)&&Trace.callbacks==callbacks+(!present&&mode!=1?1:0)&&Trace.overrides==overrides+(mode!=1&&operation==1?1:0));
            require(Trace.receiver==(mode==1?null:box)&&Trace.seenFactory==(mode==1?null:factory)&&Trace.seenMessage==(mode==1?null:message)&&Trace.payload==(mode==1?sentinel:payload)&&Trace.value==(mode==1?sentinel:desiredValue));
            Throwable wanted=mode==3||mode==8?runtime:mode==6||mode==10?error:mode==9||mode==11?checked:null;
            if(wanted!=null)require(failure==wanted&&result==sentinel);
            else if(mode==1||mode==4)require(failure instanceof NullPointerException&&result==sentinel);
            else if(mode==2)require(failure instanceof NullPointerException&&result==null);
            else if(mode==5)require(failure instanceof ClassCastException&&result==sentinel);
            else if(mode==7)require(failure instanceof ClassCastException&&result==(route>=2||operation==1?sentinel:desiredValue));
            else require(failure==null&&result==desiredValue&&value==desiredValue);
            require(Trace.thrown==(mode==3?runtime:mode==9?checked:mode==10?error:null));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
