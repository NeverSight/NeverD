import fixture.*;
import java.util.Map;
import java.util.SortedMap;
import java.util.TreeMap;
import java.util.Set;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ScopedMapInitializerUpcastHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned scoped map initializer upcast");}
    private static final class Input extends TreeMap<String,Object> {
        public Set<Map.Entry<String,Object>> entrySet(){Trace.reads++;Trace.readSource=this;Trace.readAction.run();return super.entrySet();}
    }
    @SuppressWarnings("unchecked")
    private static Core<String,Object> call(int route,int operation,SortedMap<String,Object> value)throws Throwable {
        if(route==0)return operation==0?new Core<String,Object>(value):new Core<String,Object>(value,7);
        if(route==1)return operation==0?ScopedMapInitializerUpcastCaller.first(value):ScopedMapInitializerUpcastCaller.second(value);
        if(route==2){try{return operation==0?Core.class.getConstructor(SortedMap.class).newInstance(value):Core.class.getConstructor(SortedMap.class,int.class).newInstance(value,7);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,SortedMap.class)).invokeExact(value);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,SortedMap.class,int.class)).invokeExact(value,7);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final SortedMap<String,Object> value=mode==1?null:new Input(),other=new TreeMap<String,Object>();
        final Object first=new Object(),second=new Object();if(value!=null){value.put("a",first);value.put("b",second);}
        final RuntimeException runtime=new IllegalArgumentException("owned action");final Error error=new Error("owned error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode==6?1:mode==7?2:mode==8?3:mode==9?4:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),read=new Object(),allocated=new Object(),parented=new Object();Trace.events=0;Trace.created=null;Trace.selected=null;Trace.parent=null;Trace.source=null;Trace.readSource=null;Trace.dynamicSource=value;Trace.readPayload=sentinel;Trace.allocationPayload=sentinel;Trace.payload=sentinel;Trace.key=mode==10?Integer.valueOf(3):mode==11?null:new String("owned-key");
            final int defaults=Trace.defaults,parents=Trace.parents,actions=Trace.actions,allocations=Trace.allocations,reads=Trace.reads,wrong=Trace.wrong;
            Trace.readAction=new Runnable(){public void run(){require(Trace.events==0&&Trace.created==null&&Trace.parent==null&&Trace.readSource==value);Trace.readPayload=read;Trace.dynamicSource=other;if(mode==2)throw runtime;if(mode==3)throw error;}};
            Trace.allocateAction=new Runnable(){public void run(){Trace.allocations++;require(Trace.events==1&&Trace.created!=null&&Trace.source==value&&Trace.readSource==value&&Trace.dynamicSource==other&&Trace.created.get("a")==first&&Trace.created.get("b")==second);Trace.allocationPayload=allocated;if(mode==4)throw runtime;}};
            Trace.parentAction=new Runnable(){public void run(){Trace.actions++;require(Trace.events==12&&Trace.parent.value==Trace.created&&Trace.selected==Trace.created&&Trace.created.get("a")==first&&Trace.created.get("b")==second);Trace.payload=parented;((Map)Trace.created).put("after",parented);if(mode==5)throw error;}};
            Core<String,Object> result=null;String key=null;Throwable failure=null;try{result=call(route,operation,value);key=result.key();int length=key.length();require(length==9);}catch(Throwable caught){failure=caught;}
            boolean readReached=mode!=1,allocationReached=mode!=1&&mode!=2&&mode!=3,allocationActionReached=allocationReached&&mode!=6,parentReached=allocationActionReached&&mode!=4,parentActionReached=parentReached&&mode!=7;
            long events=!allocationReached?0:mode==4||mode==6?1:mode==5||mode==7?12:mode==8?123:1234;
            require(Trace.events==events&&Trace.defaults==defaults+(allocationReached?1:0)&&Trace.allocations==allocations+(allocationActionReached?1:0)&&Trace.parents==parents+(parentReached?1:0)&&Trace.actions==actions+(parentActionReached?1:0)&&Trace.reads==reads+(readReached?1:0)&&Trace.wrong==wrong);
            Throwable expected=mode==2||mode==4?runtime:mode==3||mode==5?error:mode>=6&&mode<=9?Trace.failure:null;
            if(mode==1)require(failure instanceof NullPointerException&&result==null);
            else if(expected!=null)require(failure==expected&&result==null);
            else if(mode==10)require(failure instanceof ClassCastException&&result!=null);
            else if(mode==11)require(failure instanceof NullPointerException&&result!=null&&key==null);
            else require(failure==null&&result!=null&&key==Trace.key);
            require(Trace.readPayload==(readReached?read:sentinel)&&Trace.allocationPayload==(allocationActionReached?allocated:sentinel)&&Trace.payload==(parentActionReached?parented:sentinel));
            require((Trace.created!=null)==allocationReached&&(Trace.parent!=null)==parentActionReached&&Trace.readSource==(readReached?value:null));
            if(allocationReached){require(Trace.source==value&&Trace.created.get("a")==first&&Trace.created.get("b")==second);require(Trace.created.size()==(parentActionReached?3:2));}
            if(parentActionReached)require(Trace.parent.value==Trace.created&&Trace.selected==Trace.created&&Trace.created.get("after")==parented);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
