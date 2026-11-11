import fixture.*;
import java.util.Map;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class PrimitiveMapSuperHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned primitive map super");}
    @SuppressWarnings("unchecked")
    private static Core<String,Object> call(int route,int operation,int capacity,float factor)throws Throwable {
        if(route==0)return operation==0?new Core<String,Object>(capacity,factor):new Core<String,Object>(factor,capacity);
        if(route==1)return operation==0?PrimitiveMapSuperCaller.first(capacity,factor):PrimitiveMapSuperCaller.second(capacity,factor);
        if(route==2){try{return operation==0?Core.class.getConstructor(int.class,float.class).newInstance(capacity,factor):Core.class.getConstructor(float.class,int.class).newInstance(factor,capacity);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,int.class,float.class)).invokeExact(capacity,factor);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,float.class,int.class)).invokeExact(factor,capacity);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final int capacity=mode==3?-1:13;final float factor=mode==1?-0.0f:mode==2?Float.intBitsToFloat(0x7fc01234):0.625f;
        final RuntimeException runtime=new IllegalArgumentException("owned action");final Error error=new Error("owned error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode==6?1:mode==7?2:mode==8?3:mode==9?4:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object(),allocatedPayload=new Object();Trace.events=0;Trace.created=null;Trace.selected=null;Trace.parent=null;Trace.payload=sentinel;Trace.allocationPayload=sentinel;Trace.capacity=0;Trace.factor=0;Trace.key=mode==10?Integer.valueOf(3):mode==11?null:new String("owned-key");
            final int defaults=Trace.defaults,parents=Trace.parents,actions=Trace.actions,allocations=Trace.allocations,wrong=Trace.wrong;
            Trace.allocateAction=new Runnable(){public void run(){Trace.allocations++;require(Trace.events==1&&Trace.created!=null&&Trace.parent==null&&Trace.payload==sentinel&&Trace.capacity==capacity&&Float.floatToRawIntBits(Trace.factor)==Float.floatToRawIntBits(factor));Trace.allocationPayload=allocatedPayload;Trace.created.put(null,null);if(mode==4)throw runtime;if(mode==5)throw error;}};
            Trace.parentAction=new Runnable(){public void run(){Trace.actions++;Map selected=Trace.created;require(Trace.events==12&&Trace.selected==selected&&Trace.parent!=null&&Trace.parent.value==selected&&Trace.payload==sentinel&&selected.containsKey(null));Trace.payload=payload;selected.put("observed",payload);Trace.capacity=99;Trace.factor=1.0f;}};
            Core<String,Object> result=null;String key=null;Throwable failure=null;try{result=call(route,operation,capacity,factor);key=result.key();int length=key.length();require(length==9);}catch(Throwable caught){failure=caught;}
            boolean invalid=mode>=1&&mode<=3,allocationFailure=invalid||mode==4||mode==5||mode==6,parentReached=!allocationFailure,actionReached=parentReached&&mode!=7;
            long events=invalid?0:mode==4||mode==5||mode==6?1:mode==7?12:mode==8?123:1234;
            require(Trace.events==events&&Trace.defaults==defaults+(invalid?0:1)&&Trace.parents==parents+(parentReached?1:0)&&Trace.actions==actions+(actionReached?1:0)&&Trace.allocations==allocations+(!invalid&&mode!=6?1:0)&&Trace.wrong==wrong);
            Throwable expected=mode==4?runtime:mode==5?error:mode>=6&&mode<=9?Trace.failure:null;
            if(invalid)require(failure instanceof IllegalArgumentException&&result==null);
            else if(expected!=null)require(failure==expected&&result==null);
            else if(mode==10)require(failure instanceof ClassCastException&&result!=null);
            else if(mode==11)require(failure instanceof NullPointerException&&result!=null&&key==null);
            else require(failure==null&&result!=null&&key==Trace.key);
            require(Trace.payload==(actionReached?payload:sentinel)&&Trace.allocationPayload==(!invalid&&mode!=6?allocatedPayload:sentinel));
            require((Trace.created!=null)==(!invalid&&mode!=6));
            if(actionReached)require(Trace.selected==Trace.created&&Trace.parent.value==Trace.created&&Trace.selected.get("observed")==payload);
            else require(Trace.parent==null&&Trace.selected==null);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
