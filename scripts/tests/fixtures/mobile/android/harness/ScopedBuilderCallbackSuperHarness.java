import fixture.*;
import java.util.Map;
import java.util.HashMap;
import java.util.Set;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ScopedBuilderCallbackSuperHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned scoped builder callback super");}
    private static final class Input extends HashMap<String,Object>{public Set<Map.Entry<String,Object>> entrySet(){throw new AssertionError("unexpected owned traversal");}}
    private static final class FutureBuilder extends Builder<String,Object> {
        FutureBuilder(Map<String,Object> value){super(value);}
        @SuppressWarnings("unchecked") public Map<String,Object> build(){Trace.views++;Trace.builds++;Trace.builder=this;Trace.hit(1);Map<String,Object> result=(Map<String,Object>)Trace.futureResult;Trace.buildAction.run();return result;}
    }
    @SuppressWarnings("unchecked")
    private static Core<String,Object> call(int route,int operation,Builder<String,Object> value)throws Throwable {
        if(route==0)return operation==0?new Core<String,Object>(value):new Core<String,Object>(value,7);
        if(route==1)return operation==0?ScopedBuilderCallbackSuperCaller.first(value):ScopedBuilderCallbackSuperCaller.second(value);
        if(route==2){try{return operation==0?Core.class.getConstructor(Builder.class).newInstance(value):Core.class.getConstructor(Builder.class,int.class).newInstance(value,7);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Builder.class)).invokeExact(value);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Builder.class,int.class)).invokeExact(value,7);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Map<String,Object> decoy=new Input(),first=new Input(),future=new Input(),mutated=new Input();
        final Builder<String,Object> builder=mode==1?null:operation==0?new Builder<String,Object>(decoy):new FutureBuilder(decoy);
        final Map<String,Object> selected=operation==0?first:future;
        final RuntimeException runtime=new IllegalArgumentException("owned action");final Error error=new Error("owned error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode==6?1:mode==7?2:mode==8?3:mode==9?6:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),built=new Object(),allocated=new Object(),parented=new Object(),called=new Object(),input=new Object(),later=new Object();
            final Object key=mode==10?Integer.valueOf(3):mode==11?null:new String("owned-key");
            Trace.events=0;Trace.builder=null;Trace.adapter=null;Trace.parent=null;Trace.selected=null;Trace.mapResult=first;Trace.futureResult=future;Trace.callbackResult=key;Trace.callbackInput=null;Trace.input=input;Trace.buildPayload=sentinel;Trace.allocationPayload=sentinel;Trace.parentPayload=sentinel;Trace.callbackPayload=sentinel;
            if(builder!=null)builder.value=decoy;
            final int builds=Trace.builds,buildActions=Trace.buildActions,views=Trace.views,adapters=Trace.adapters,allocations=Trace.allocations,parents=Trace.parents,actions=Trace.actions,calls=Trace.calls,callbackActions=Trace.callbackActions,children=Trace.children,wrong=Trace.wrong;
            Trace.buildAction=new Runnable(){public void run(){Trace.buildActions++;require(Trace.events==1&&Trace.builder==builder&&Trace.adapter==null&&Trace.parent==null);Trace.buildPayload=built;builder.value=mutated;Trace.mapResult=mutated;Trace.futureResult=mutated;if(mode==2)throw runtime;}};
            Trace.allocateAction=new Runnable(){public void run(){Trace.allocations++;require(Trace.events==12&&Trace.builder==builder&&Trace.adapter!=null&&Trace.parent==null&&Trace.buildPayload==built);Trace.allocationPayload=allocated;if(mode==3)throw error;}};
            Trace.parentAction=new Runnable(){public void run(){Trace.actions++;require(Trace.events==123&&Trace.selected==selected&&Trace.parent.value==selected&&Trace.parent.function==Trace.adapter&&Trace.calls==calls);Trace.parentPayload=parented;((Map)selected).put(Integer.valueOf(5),parented);if(mode==4)throw runtime;}};
            Trace.callbackAction=new Runnable(){public void run(){Trace.callbackActions++;require(Trace.events==1234&&Trace.callbackInput==input&&Trace.parent.function==Trace.adapter&&Trace.parentPayload==parented);Trace.callbackPayload=called;Trace.callbackResult=later;if(mode==5)throw error;}};
            Core<String,Object> result=null;String value=null;Throwable failure=null;try{result=call(route,operation,builder);value=result.key;int length=value.length();require(length==9);}catch(Throwable caught){failure=caught;}
            boolean buildReached=mode!=1,buildActionReached=buildReached&&mode!=6,allocationReached=buildActionReached&&mode!=2,allocationActionReached=allocationReached&&mode!=7,parentReached=allocationActionReached&&mode!=3,parentActionReached=parentReached&&mode!=8,callbackReached=parentActionReached&&mode!=4,childReached=callbackReached&&mode!=5;
            long events=mode==1?0:mode==2||mode==6?1:mode==3||mode==7?12:mode==4||mode==8?123:mode==5?1234:123456;
            require(Trace.events==events&&Trace.builds==builds+(buildReached?1:0)&&Trace.buildActions==buildActions+(buildActionReached?1:0)&&Trace.views==views+(buildReached&&operation==1?1:0)&&Trace.adapters==adapters+(allocationReached?1:0)&&Trace.allocations==allocations+(allocationActionReached?1:0)&&Trace.parents==parents+(parentReached?1:0)&&Trace.actions==actions+(parentActionReached?1:0)&&Trace.calls==calls+(callbackReached?1:0)&&Trace.callbackActions==callbackActions+(callbackReached?1:0)&&Trace.children==children+(childReached?1:0)&&Trace.wrong==wrong);
            Throwable expected=mode==2||mode==4?runtime:mode==3||mode==5?error:mode>=6&&mode<=9?Trace.failure:null;
            if(mode==1)require(failure instanceof NullPointerException&&result==null);
            else if(expected!=null)require(failure==expected&&result==null);
            else if(mode==10)require(failure instanceof ClassCastException&&result!=null);
            else if(mode==11)require(failure instanceof NullPointerException&&result!=null&&value==null);
            else require(failure==null&&result!=null&&value==key);
            require(Trace.buildPayload==(buildActionReached?built:sentinel)&&Trace.allocationPayload==(allocationActionReached?allocated:sentinel)&&Trace.parentPayload==(parentActionReached?parented:sentinel)&&Trace.callbackPayload==(callbackReached?called:sentinel));
            require((Trace.adapter!=null)==allocationReached&&(Trace.parent!=null)==parentReached&&Trace.builder==(buildReached?builder:null));
            if(parentReached){require(Trace.selected==selected&&Trace.parent.value==selected&&Trace.parent.function==Trace.adapter&&Trace.parent.key==(childReached?key:null));if(parentActionReached)require(((Map)selected).get(Integer.valueOf(5))==parented);}
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
