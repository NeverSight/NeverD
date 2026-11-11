import fixture.*;
import java.util.Map;
import java.util.HashMap;
import java.util.Set;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ClosedMapSuperHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned closed map super");}
    private static final class Input extends HashMap<String,Object>{public Set<Map.Entry<String,Object>> entrySet(){throw new AssertionError("unexpected owned traversal");}}
    private static Core call(int route,int operation,Map<String,Object> value)throws Throwable {
        if(route==0)return operation==0?new Core(value):new Core(value,7);
        if(route==1)return operation==0?ClosedMapSuperCaller.first(value):ClosedMapSuperCaller.second(value);
        if(route==2){try{return operation==0?Core.class.getConstructor(Map.class).newInstance(value):Core.class.getConstructor(Map.class,int.class).newInstance(value,7);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Map.class)).invokeExact(value);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Map.class,int.class)).invokeExact(value,7);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Map<String,Object> value=mode==1?null:new Input();final boolean fresh=operation==0?value==null:value!=null;
        final RuntimeException runtime=new IllegalArgumentException("owned action");final Error error=new Error("owned error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode==6?1:mode==7?2:mode==8?3:mode==9?4:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object(),allocatedPayload=new Object();Trace.events=0;Trace.created=null;Trace.selected=null;Trace.parent=null;Trace.payload=sentinel;Trace.allocationPayload=sentinel;Trace.key=mode==10?Integer.valueOf(3):mode==11?null:new String("owned-key");
            final int defaults=Trace.defaults,parents=Trace.parents,actions=Trace.actions,allocations=Trace.allocations,wrong=Trace.wrong;
            Trace.allocateAction=new Runnable(){public void run(){Trace.allocations++;require(Trace.events==1&&Trace.created!=null&&Trace.parent==null&&Trace.payload==sentinel);Trace.allocationPayload=allocatedPayload;if(mode==4)throw runtime;if(mode==5)throw error;}};
            Trace.parentAction=new Runnable(){public void run(){Trace.actions++;Map selected=fresh?Trace.created:value;require(Trace.events==(fresh?12:2)&&Trace.selected==selected&&Trace.parent!=null&&Trace.parent.value==selected&&Trace.payload==sentinel);Trace.payload=payload;if(selected!=null)selected.put("observed",payload);if(mode==4)throw runtime;if(mode==5)throw error;}};
            Core result=null;String key=null;Throwable failure=null;try{result=call(route,operation,value);key=result.key();int length=key.length();require(length==9);}catch(Throwable caught){failure=caught;}
            boolean allocationFailure=fresh&&(mode==4||mode==5||mode==6),parentReached=!allocationFailure,actionReached=parentReached&&mode!=7;
            long events=fresh?mode==4||mode==5||mode==6?1:mode==4||mode==5||mode==7?12:mode==8?123:1234:mode==4||mode==5||mode==7?2:mode==8?23:234;
            require(Trace.events==events&&Trace.defaults==defaults+(fresh?1:0)&&Trace.parents==parents+(parentReached?1:0)&&Trace.actions==actions+(actionReached?1:0)&&Trace.allocations==allocations+(fresh&&mode!=6?1:0)&&Trace.wrong==wrong);
            Throwable expected=mode==4?runtime:mode==5?error:mode==6&&fresh||mode==7||mode==8||mode==9?Trace.failure:null;
            if(expected!=null)require(failure==expected&&result==null);
            else if(mode==10)require(failure instanceof ClassCastException&&result!=null);
            else if(mode==11)require(failure instanceof NullPointerException&&result!=null&&key==null);
            else require(failure==null&&result!=null&&key==Trace.key);
            require(Trace.payload==(actionReached?payload:sentinel)&&Trace.allocationPayload==(fresh&&mode!=6?allocatedPayload:sentinel));
            require((Trace.created!=null)==(fresh&&mode!=6));
            if(actionReached){Map selected=fresh?Trace.created:value;require(Trace.selected==selected&&Trace.parent.value==selected);if(selected!=null)require(selected.get("observed")==payload);}
            else require(Trace.parent==null&&Trace.selected==null);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
