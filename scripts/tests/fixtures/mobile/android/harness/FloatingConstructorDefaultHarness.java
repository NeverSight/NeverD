import fixture.*;
import java.util.Map;
import java.util.HashMap;
import java.util.Set;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class FloatingConstructorDefaultHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned floating constructor default");}
    private static final class Input extends HashMap<String,Object>{public Set<Map.Entry<String,Object>> entrySet(){throw new AssertionError("unexpected owned traversal");}}
    @SuppressWarnings("unchecked")
    private static Core<String,Object> call(int route,int operation,Map<String,Object> value)throws Throwable {
        if(route==0)return operation==0?new Core<String,Object>(value):new Core<String,Object>(value,7);
        if(route==1)return operation==0?FloatingConstructorDefaultCaller.first(value):FloatingConstructorDefaultCaller.second(value);
        if(route==2){try{return operation==0?Core.class.getConstructor(Map.class).newInstance(value):Core.class.getConstructor(Map.class,int.class).newInstance(value,7);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Map.class)).invokeExact(value);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Map.class,int.class)).invokeExact(value,7);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Map<String,Object> value=mode==1?null:new Input();
        final RuntimeException runtime=new IllegalArgumentException("owned action");final Error error=new Error("owned error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode==6?2:mode==7?3:mode==8?4:mode==9?6:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object(),childPayload=new Object();Trace.events=0;Trace.selected=null;Trace.parent=null;Trace.child=null;Trace.payload=sentinel;Trace.allocationPayload=sentinel;Trace.scalar32=3.5f;Trace.scalar64=4.5d;Trace.key=mode==10?Integer.valueOf(3):mode==11?null:new String("owned-key");
            final int parents=Trace.parents,actions=Trace.actions,children=Trace.children,childActions=Trace.childActions,wrappers=Trace.wrappers,wrong=Trace.wrong;
            Trace.parentAction=new Runnable(){public void run(){Trace.actions++;require(Trace.events==2&&Trace.selected==value&&Trace.parent!=null&&Trace.parent.value==value&&Trace.child==null&&Trace.payload==sentinel);Trace.payload=payload;if(value!=null)((Map)value).put(Integer.valueOf(5),payload);if(mode==2)throw runtime;if(mode==3)throw error;}};
            Trace.childAction=new Runnable(){public void run(){Trace.childActions++;require(Trace.events==234&&Trace.child==Trace.parent&&Trace.child.value==value&&Trace.payload==payload);require(Float.floatToRawIntBits(Trace.scalar32)==Float.floatToRawIntBits(operation==0?0.625f:3.5f)&&Double.doubleToRawLongBits(Trace.scalar64)==Double.doubleToRawLongBits(operation==1?-0.0d:4.5d));Trace.allocationPayload=childPayload;Trace.scalar32=11;Trace.scalar64=12;if(mode==4)throw runtime;if(mode==5)throw error;}};
            Core<String,Object> result=null;String key=null;Throwable failure=null;try{result=call(route,operation,value);key=result.key();int length=key.length();require(length==9);}catch(Throwable caught){failure=caught;}
            boolean parentActionReached=mode!=6,targetReached=mode!=2&&mode!=3&&mode!=6&&mode!=7,childActionReached=targetReached&&mode!=8,wrapperReached=targetReached&&mode!=4&&mode!=5&&mode!=8;
            long events=mode==2||mode==3||mode==6?2:mode==7?23:mode==4||mode==5||mode==8?234:23456;
            require(Trace.events==events&&Trace.parents==parents+1&&Trace.actions==actions+(parentActionReached?1:0)&&Trace.children==children+(targetReached?1:0)&&Trace.childActions==childActions+(childActionReached?1:0)&&Trace.wrappers==wrappers+(wrapperReached?1:0)&&Trace.wrong==wrong);
            Throwable expected=mode==2||mode==4?runtime:mode==3||mode==5?error:mode>=6&&mode<=9?Trace.failure:null;
            if(expected!=null)require(failure==expected&&result==null);
            else if(mode==10)require(failure instanceof ClassCastException&&result!=null);
            else if(mode==11)require(failure instanceof NullPointerException&&result!=null&&key==null);
            else require(failure==null&&result!=null&&key==Trace.key);
            require(Trace.payload==(parentActionReached?payload:sentinel)&&Trace.allocationPayload==(childActionReached?childPayload:sentinel));
            require((Trace.child!=null)==targetReached&&((Trace.parent!=null)==parentActionReached));
            if(parentActionReached){require(Trace.selected==value&&Trace.parent.value==value);if(value!=null)require(((Map)value).get(Integer.valueOf(5))==payload);}
            float expected32=childActionReached?11:targetReached&&operation==0?0.625f:3.5f;double expected64=childActionReached?12:targetReached&&operation==1?-0.0d:4.5d;
            require(Float.floatToRawIntBits(Trace.scalar32)==Float.floatToRawIntBits(expected32)&&Double.doubleToRawLongBits(Trace.scalar64)==Double.doubleToRawLongBits(expected64));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
